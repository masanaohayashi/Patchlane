#include "AudioCore.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <array>
#include <vector>
#include <memory>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <thread>
#include <chrono>

namespace {
constexpr int N = 4, Capacity = 65536, MaxFrames = 8192;
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
struct Ring {
    // Atomic samples make even a lapped reader data-race free. Readers detect
    // overrun and re-prime instead of blocking the device callback.
    std::array<std::atomic<float>, Capacity * 2> samples{};
    std::atomic<uint64_t> written{0};
    void push(const float *p, int n) {
        auto w = written.load(std::memory_order_relaxed);
        for (int f=0; f<n; ++f) for (int c=0; c<2; ++c)
            samples[((w+f)%Capacity)*2+c].store(std::isfinite(p[f*2+c]) ? p[f*2+c] : 0, std::memory_order_relaxed);
        written.store(w+n, std::memory_order_release);
    }
    float at(uint64_t f,int c) const { return samples[(f%Capacity)*2+c].load(std::memory_order_relaxed); }
    void clear() { written.store(0); }
};
struct Cursor { double position=0; bool ready=false; float gain=0; };
struct Input {
    uint32_t device=0; int left=0,right=1; double rate=48000;
    AudioUnit unit=nullptr; Ring ring;
    std::atomic<float> gain{1}, peak{0};
    std::atomic<bool> route[N]{};
    std::vector<float> capture; std::array<float,MaxFrames*2> stereo{};
    uint32_t channels=2;
};
struct Output {
    uint32_t device=0; int left=0,right=1; double rate=48000; AudioUnit unit=nullptr;
    std::atomic<float> gain{1},peak{0};
    Cursor cursor[N]; float master=0;
    std::atomic<uint32_t> callbackFrames{0};
};
uint32_t channels(uint32_t id, bool input) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration,input?kAudioDevicePropertyScopeInput:kAudioDevicePropertyScopeOutput,kAudioObjectPropertyElementMain};
    UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&size)!=noErr || !size) return 0;
    std::vector<uint8_t> data(size); auto *list=reinterpret_cast<AudioBufferList*>(data.data());
    if(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,list)!=noErr) return 0;
    uint32_t total=0; for(UInt32 i=0;i<list->mNumberBuffers;++i)total+=list->mBuffers[i].mNumberChannels;
    return total;
}
double sampleRate(uint32_t id) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    double r=0; UInt32 s=sizeof(r); AudioObjectGetPropertyData(id,&a,0,nullptr,&s,&r); return r;
}
void stringProperty(uint32_t id, AudioObjectPropertySelector selector, char *out, int size) {
    AudioObjectPropertyAddress a{selector,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    CFStringRef value=nullptr; UInt32 s=sizeof(value);
    if(AudioObjectGetPropertyData(id,&a,0,nullptr,&s,&value)==noErr && value) {
        CFStringGetCString(value,out,size,kCFStringEncodingUTF8); CFRelease(value);
    }
}
void dispose(AudioUnit &u) { if(u) { AudioOutputUnitStop(u); AudioUnitUninitialize(u); AudioComponentInstanceDispose(u); u=nullptr; } }
AudioStreamBasicDescription format(double rate,int ch) {
    AudioStreamBasicDescription f{}; f.mSampleRate=rate; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagsNativeFloatPacked; f.mBytesPerPacket=f.mBytesPerFrame=ch*4;
    f.mFramesPerPacket=1; f.mChannelsPerFrame=ch; f.mBitsPerChannel=32; return f;
}
void peakStore(std::atomic<float>&p, float v) {
    float old=p.load(std::memory_order_relaxed);
    // Meter accuracy must never make audio wait for the UI's exchange(0).
    if(v>old) p.compare_exchange_strong(old,v,std::memory_order_relaxed);
}
}
struct LCEngine {
    Input input[N]; Output output[N]; Output offline[N];
    struct Context { LCEngine *engine; int index; } contexts[N];
    char error[256]{}; bool running=false;
    int blockFrames=32, delayFrames=0; double systemRate=44100;
    struct Original { uint32_t device; double rate; uint32_t frames; };
    std::vector<Original> originals;
    template<typename T> bool readDevice(uint32_t device, AudioObjectPropertySelector selector, T &value) {
        AudioObjectPropertyAddress a{selector,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        UInt32 size=sizeof(value);
        return check(AudioObjectGetPropertyData(device,&a,0,nullptr,&size,&value),"Read device timing");
    }
    template<typename T> bool setDevice(uint32_t device, AudioObjectPropertySelector selector, T value) {
        AudioObjectPropertyAddress a{selector,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        if(!check(AudioObjectSetPropertyData(device,&a,0,nullptr,sizeof(value),&value),"Apply requested device timing"))return false;
        for(int i=0;i<100;++i) {
            T actual{}; if(!readDevice(device,selector,actual))return false;
            if(actual==value)return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::snprintf(error,sizeof(error),"Device %u did not accept requested timing (%g).",device,double(value));return false;
    }
    bool prepareDevice(uint32_t device) {
        if(!device)return true;
        for(auto &state:originals)if(state.device==device)return true;
        Original state{device,0,0};
        if(!readDevice(device,kAudioDevicePropertyNominalSampleRate,state.rate) ||
           !readDevice(device,kAudioDevicePropertyBufferFrameSize,state.frames))return false;
        originals.push_back(state);
        if(state.rate!=systemRate && !setDevice(device,kAudioDevicePropertyNominalSampleRate,systemRate))return false;
        if(state.frames!=uint32_t(blockFrames) && !setDevice(device,kAudioDevicePropertyBufferFrameSize,uint32_t(blockFrames)))return false;
        return true;
    }
    void restoreDevices() {
        char saved[256];std::memcpy(saved,error,sizeof(error));
        for(auto &state:originals) {
            double rate=0;uint32_t frames=0;
            // Do not undo a subsequent setting change made by another client.
            if(readDevice(state.device,kAudioDevicePropertyBufferFrameSize,frames) && frames==uint32_t(blockFrames) && frames!=state.frames)
                setDevice(state.device,kAudioDevicePropertyBufferFrameSize,state.frames);
            if(readDevice(state.device,kAudioDevicePropertyNominalSampleRate,rate) && rate==systemRate && rate!=state.rate)
                setDevice(state.device,kAudioDevicePropertyNominalSampleRate,state.rate);
        }
        originals.clear();std::memcpy(error,saved,sizeof(error));
    }
    LCEngine() { for(int i=0;i<N;++i) contexts[i]={this,i}; }
    bool check(OSStatus s,const char *operation) {
        if(!s)return true;
        std::snprintf(error,sizeof(error),"%s: Core Audio error %d",operation,(int)s); return false;
    }
    void mix(Output &o,int bus,float *dest,int frames,double rate) {
        std::fill(dest,dest+frames*2,0.f);
        for(int i=0;i<N;++i) {
            auto &in=input[i]; auto &c=o.cursor[i];
            const auto w=in.ring.written.load(std::memory_order_acquire);
            // Read the newest complete callback block; never accumulate a fixed
            // time cushion. Unequal rates need one interpolation lookahead sample.
            const double target=std::ceil(frames*in.rate/rate)+(in.rate==rate ? 0:1)+delayFrames*in.rate/systemRate;
            if(!c.ready || c.position< double(w)-Capacity+2 || c.position>double(w)) {
                if(!c.ready && double(w)-c.position<target) { continue; }
                if(double(w)<target) { c.ready=false; continue; }
                c.position=double(w)-target; c.ready=true;
            }
            // Smoothly track clock drift rather than changing hardware clocks.
            const double errorFrames=double(w)-c.position-target;
            const double adjustment=std::clamp(errorFrames/(in.rate*2.0),-0.003,0.003);
            const double step=in.rate/rate*(1+adjustment);
            const float goal=in.route[bus].load(std::memory_order_relaxed)?in.gain.load(std::memory_order_relaxed):0;
            const float slew=1.f-std::exp(-1.f/(float(rate)*0.005f));
            for(int f=0;f<frames;++f) {
                c.gain+=(goal-c.gain)*slew;
                auto p=uint64_t(c.position); float t=float(c.position-p);
                if(p>=w || (t>0 && p+1>=w)) { c.position=double(w); c.ready=false; break; }
                for(int ch=0;ch<2;++ch) {
                    float a=in.ring.at(p,ch),b=t>0 ? in.ring.at(p+1,ch):a;
                    dest[f*2+ch]+=(a+(b-a)*t)*c.gain;
                }
                c.position+=step;
            }
        }
        float peak=0,goal=output[bus].gain.load(std::memory_order_relaxed);
        const float slew=1.f-std::exp(-1.f/(float(rate)*0.005f));
        for(int f=0;f<frames;++f) {
            o.master+=(goal-o.master)*slew;
            for(int c=0;c<2;++c) { float &v=dest[f*2+c]; v*=o.master; peak=std::max(peak,std::abs(v)); v=std::clamp(v,-1.f,1.f); }
        }
        peakStore(output[bus].peak,peak);
    }
};
namespace {
OSStatus capture(void *ref,AudioUnitRenderActionFlags *flags,const AudioTimeStamp *ts,UInt32,UInt32 frames,AudioBufferList*) {
    auto &in=*static_cast<Input*>(ref);
    if(frames>MaxFrames)return kAudioUnitErr_TooManyFramesToProcess;
    AudioBufferList list{}; list.mNumberBuffers=1;
    list.mBuffers[0]={in.channels,frames*in.channels*4,in.capture.data()};
    OSStatus s=AudioUnitRender(in.unit,flags,ts,1,frames,&list); if(s)return s;
    float peak=0;
    for(UInt32 f=0;f<frames;++f) {
        float l=in.capture[f*in.channels+in.left],r=in.capture[f*in.channels+in.right];
        in.stereo[f*2]=l; in.stereo[f*2+1]=r; peak=std::max(peak,std::max(std::abs(l),std::abs(r)));
    }
    in.ring.push(in.stereo.data(),frames); peakStore(in.peak,peak*in.gain.load(std::memory_order_relaxed)); return noErr;
}
OSStatus render(void *ref,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList *data) {
    auto &c=*static_cast<LCEngine::Context*>(ref); auto &o=c.engine->output[c.index];
    if(!data || data->mNumberBuffers!=1 || data->mBuffers[0].mNumberChannels!=2)return kAudio_ParamError;
    o.callbackFrames.store(frames,std::memory_order_relaxed);
    c.engine->mix(o,c.index,static_cast<float*>(data->mBuffers[0].mData),frames,o.rate); return noErr;
}
bool setup(LCEngine *e,AudioUnit &u,uint32_t device,bool input,double rate,int ch,void *ref) {
    AudioComponentDescription d{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};
    auto component=AudioComponentFindNext(nullptr,&d);
    if(!component)return e->check(-1,"AUHAL unavailable");
    if(!e->check(AudioComponentInstanceNew(component,&u),"Create AUHAL"))return false;
    UInt32 one=1,zero=0,max=MaxFrames;
    if(input) {
        if(!e->check(AudioUnitSetProperty(u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&one,4),"Enable input"))return false;
        if(!e->check(AudioUnitSetProperty(u,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&zero,4),"Disable output"))return false;
    }
    if(!e->check(AudioUnitSetProperty(u,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,4),"Select device"))return false;
    auto f=format(rate,ch);
    if(!e->check(AudioUnitSetProperty(u,kAudioUnitProperty_StreamFormat,input?kAudioUnitScope_Output:kAudioUnitScope_Input,input?1:0,&f,sizeof(f)),"Set audio format"))return false;
    if(!input) {
        auto &context=*static_cast<LCEngine::Context*>(ref);
        auto &out=e->output[context.index];
        const auto count=channels(device,false);
        if(out.left>=int(count)||out.right>=int(count)||out.left==out.right)return e->check(-1,"Output channel unavailable");
        std::vector<SInt32> mapping(count,-1);mapping[out.left]=0;mapping[out.right]=1;
        if(!e->check(AudioUnitSetProperty(u,kAudioOutputUnitProperty_ChannelMap,kAudioUnitScope_Output,0,mapping.data(),UInt32(mapping.size()*sizeof(SInt32))),"Set output channels"))return false;
    }
    if(!e->check(AudioUnitSetProperty(u,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&max,4),"Set frame limit"))return false;
    AURenderCallbackStruct cb{input?capture:render,ref};
    if(!e->check(AudioUnitSetProperty(u,input?kAudioOutputUnitProperty_SetInputCallback:kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Global,0,&cb,sizeof(cb)),"Set callback"))return false;
    return e->check(AudioUnitInitialize(u),"Initialize audio");
}
}
extern "C" {
int lc_devices(LCDevice *dest,int capacity) {
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 s=0; if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,nullptr,&s))return 0;
    std::vector<AudioDeviceID> ids(s/sizeof(AudioDeviceID));
    if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,nullptr,&s,ids.data()))return 0;
    int count=0;
    for(auto id:ids) {
        auto in=channels(id,true),out=channels(id,false); if(!in&&!out)continue;
        if(dest && count<capacity) { auto &d=dest[count]; d={}; d.id=id;d.inputs=in;d.outputs=out;d.rate=sampleRate(id);stringProperty(id,kAudioObjectPropertyName,d.name,256);stringProperty(id,kAudioDevicePropertyDeviceUID,d.uid,256); }
        ++count;
    }
    return count;
}
LCEngine *lc_create(){ return new LCEngine; }
void lc_destroy(LCEngine *e){if(e){lc_stop(e);delete e;}}
int lc_config_input(LCEngine *e,int i,uint32_t d,int l,int r){if(!e||e->running||i<0||i>=N||l<0||r<0)return -1;e->input[i].device=d;e->input[i].left=l;e->input[i].right=r;return 0;}
int lc_config_output(LCEngine *e,int b,uint32_t d){if(!e||e->running||b<0||b>=N)return -1;e->output[b].device=d;return 0;}
int lc_config_output_channels(LCEngine *e,int bus,int left,int right) {
    if(!e||e->running||bus<0||bus>=N||left<0||right<0||left==right)return -1;
    e->output[bus].left=left;e->output[bus].right=right;return 0;
}
void lc_input_gain(LCEngine *e,int i,float g){if(e&&i>=0&&i<N)e->input[i].gain.store(std::isfinite(g)?std::clamp(g,0.f,64.f):0);}
void lc_output_gain(LCEngine *e,int b,float g){if(e&&b>=0&&b<N)e->output[b].gain.store(std::isfinite(g)?std::clamp(g,0.f,1.f):0);}
void lc_route(LCEngine *e,int i,int b,int v){if(e&&i>=0&&i<N&&b>=0&&b<N)e->input[i].route[b].store(v!=0);}
int lc_config_timing(LCEngine *e,int frames,double rate) {
    if(!e||e->running||frames<32||frames>2048||(frames&(frames-1))||
       (rate!=44100 && rate!=48000 && rate!=88200 && rate!=96000))return -1;
    e->blockFrames=frames;e->systemRate=rate;return 0;
}
uint32_t lc_output_callback_frames(LCEngine *e,int bus) {return e&&bus>=0&&bus<N?e->output[bus].callbackFrames.load():0;}
int lc_config_delay(LCEngine *e,int frames) {
    if(!e||e->running||frames<0||frames>32768)return -1;
    e->delayFrames=frames;return 0;
}
int lc_start(LCEngine *e){
    if(e->running)return 0; e->error[0]=0;
    for(int i=0;i<N;++i) {
        e->output[i].callbackFrames.store(0);
        if(!e->prepareDevice(e->input[i].device)||!e->prepareDevice(e->output[i].device)){lc_stop(e);return -1;}
    }
    for(int i=0;i<N;++i){
        auto &in=e->input[i]; in.ring.clear(); in.peak.store(0);
        for(auto &c:e->output[i].cursor)c={}; for(auto &c:e->offline[i].cursor)c={};
        e->output[i].master=e->offline[i].master=0;
        if(in.device){
            in.rate=sampleRate(in.device);in.channels=channels(in.device,true);
            if(in.rate<8000||!in.channels||in.left>=int(in.channels)||in.right>=int(in.channels)){e->check(-1,"Input device or channel unavailable");lc_stop(e);return -1;}
            in.capture.resize(MaxFrames*in.channels);
            if(!setup(e,in.unit,in.device,true,in.rate,in.channels,&in)){lc_stop(e);return -1;}
        }
        auto &o=e->output[i];
        if(o.device){
            o.rate=sampleRate(o.device);
            if(o.rate<8000||!channels(o.device,false)){e->check(-1,"Output device unavailable");lc_stop(e);return -1;}
            if(!setup(e,o.unit,o.device,false,o.rate,2,&e->contexts[i])){lc_stop(e);return -1;}
        }
    }
    for(int i=0;i<N;++i){
        if(e->input[i].unit&&!e->check(AudioOutputUnitStart(e->input[i].unit),"Start input")){lc_stop(e);return -1;}
        if(e->output[i].unit&&!e->check(AudioOutputUnitStart(e->output[i].unit),"Start output")){lc_stop(e);return -1;}
    }
    e->running=true;return 0;
}
void lc_stop(LCEngine *e){if(!e)return;for(auto &o:e->output)dispose(o.unit);for(auto &i:e->input)dispose(i.unit);e->restoreDevices();e->running=false;}
const char *lc_error(LCEngine *e){return e->error;}
float lc_input_peak(LCEngine *e,int i){return e&&i>=0&&i<N?e->input[i].peak.exchange(0):0;}
float lc_output_peak(LCEngine *e,int i){return e&&i>=0&&i<N?e->output[i].peak.exchange(0):0;}
void lc_mix_read(LCEngine *e,int b,float *p,int n,double r){if(e&&b>=0&&b<N&&n>0&&n<=MaxFrames&&r>=8000)e->mix(e->offline[b],b,p,n,r);}
void lc_mix_reset(LCEngine *e,int b){if(e&&b>=0&&b<N){for(auto &c:e->offline[b].cursor)c={};e->offline[b].master=0;}}
void lc_test_feed(LCEngine *e,int i,const float *p,int n,double r){if(e&&!e->running&&i>=0&&i<N&&r>=8000){e->input[i].rate=r;e->input[i].ring.push(p,n);}}
}
