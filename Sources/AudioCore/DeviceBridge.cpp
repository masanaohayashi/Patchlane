#include "AudioCore.h"
#include "../../Shared/BrokerClient.hpp"
#include "../../Shared/TimedReader.hpp"
#include <mach/mach_time.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace {
constexpr auto Global=kAudioObjectPropertyScopeGlobal;
constexpr auto In=kAudioObjectPropertyScopeInput,Out=kAudioObjectPropertyScopeOutput;
void check(OSStatus status,const char *what) { if(status) throw std::runtime_error(std::string(what)+": "+std::to_string(status)); }
template<class T> T get(AudioObjectID id,UInt32 key,UInt32 scope=Global) {
    AudioObjectPropertyAddress a{key,scope,kAudioObjectPropertyElementMain}; T value{}; UInt32 size=sizeof(value);
    check(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&value),"Read bridge device property");
    if(size!=sizeof(value)) throw std::runtime_error("Unexpected bridge property size"); return value;
}
std::vector<AudioObjectID> list(AudioObjectID id,UInt32 key,UInt32 scope) {
    AudioObjectPropertyAddress a{key,scope,kAudioObjectPropertyElementMain}; UInt32 size=0;
    check(AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&size),"Read bridge stream count");
    std::vector<AudioObjectID> result(size/sizeof(AudioObjectID));
    if(size) check(AudioObjectGetPropertyData(id,&a,0,nullptr,&size,result.data()),"Read bridge streams");
    result.resize(size/sizeof(AudioObjectID)); return result;
}
template<class T> void set(AudioObjectID id,UInt32 key,T value,UInt32 scope=Global) {
    AudioObjectPropertyAddress a{key,scope,kAudioObjectPropertyElementMain};
    check(AudioObjectSetPropertyData(id,&a,0,nullptr,sizeof(value),&value),"Set bridge device property");
}
template<class T> void setTiming(AudioDeviceID id,UInt32 key,T value) {
    if(get<T>(id,key)==value) return;
    set(id,key,value);
    for(unsigned i=0;i<100;++i) {
        if(get<T>(id,key)==value) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Bridge device did not accept requested timing");
}
struct CF {
    CFTypeRef ref=nullptr;
    explicit CF(CFTypeRef v=nullptr):ref(v) {}
    ~CF() { if(ref) CFRelease(ref); }
    CF(const CF&)=delete; CF& operator=(const CF&)=delete;
};
unsigned channels(AudioDeviceID device,UInt32 scope) {
    unsigned total=0;
    for(auto stream:list(device,kAudioDevicePropertyStreams,scope)) {
        auto f=get<AudioStreamBasicDescription>(stream,kAudioStreamPropertyVirtualFormat);
        if(f.mFormatID!=kAudioFormatLinearPCM||!(f.mFormatFlags&kAudioFormatFlagIsFloat)||f.mBitsPerChannel!=32)
            throw std::runtime_error("Bridge requires Float32 device streams");
        total+=f.mChannelsPerFrame;
    }
    return total;
}
struct Channel {
    float *samples=nullptr; unsigned stride=0,frames=0;
};
Channel channel(AudioBufferList *buffers,unsigned ch) {
    if(buffers) for(unsigned b=0;b<buffers->mNumberBuffers;++b) {
        auto &buffer=buffers->mBuffers[b];
        if(ch<buffer.mNumberChannels) return {buffer.mData?static_cast<float*>(buffer.mData)+ch:nullptr,buffer.mNumberChannels,unsigned(buffer.mDataByteSize/(buffer.mNumberChannels*sizeof(float)))};
        ch-=buffer.mNumberChannels;
    }
    return {};
}
}
struct LCBridge {
    AudioDeviceID aggregate=0; AudioDeviceIOProcID proc=nullptr;
    unsigned inputLeft=0,inputRight=1,outputLeft=0,outputRight=1;
    char error[256]{};
    uint32_t requestedFrames=0; double requestedRate=0;
    struct Original { AudioDeviceID id; UInt32 frames; double rate; };
    std::vector<Original> originals;
    std::atomic<uint64_t> callbacks{0},missing{0};
    std::atomic<uint32_t> minFrames{UINT32_MAX},maxFrames{0};
    std::atomic<float> peak{0};
    void prepare(AudioDeviceID id) {
        auto old=Original{id,get<UInt32>(id,kAudioDevicePropertyBufferFrameSize),get<double>(id,kAudioDevicePropertyNominalSampleRate)};
        originals.push_back(old);
        setTiming(id,kAudioDevicePropertyNominalSampleRate,requestedRate);
        setTiming(id,kAudioDevicePropertyBufferFrameSize,requestedFrames);
    }
    void stop() {
        if(proc) { AudioDeviceStop(aggregate,proc); AudioDeviceDestroyIOProcID(aggregate,proc); proc=nullptr; }
        if(aggregate) { AudioHardwareDestroyAggregateDevice(aggregate); aggregate=0; }
        for(auto old:originals) {
            try {
                if(get<UInt32>(old.id,kAudioDevicePropertyBufferFrameSize)==requestedFrames) setTiming(old.id,kAudioDevicePropertyBufferFrameSize,old.frames);
                if(get<double>(old.id,kAudioDevicePropertyNominalSampleRate)==requestedRate) setTiming(old.id,kAudioDevicePropertyNominalSampleRate,old.rate);
            } catch(const std::exception &e) { if(!error[0]) std::snprintf(error,sizeof(error),"Restore timing: %s",e.what()); }
        }
        originals.clear();
    }
    ~LCBridge() { stop(); }
    static OSStatus callback(AudioDeviceID,const AudioTimeStamp*,const AudioBufferList *input,const AudioTimeStamp*,AudioBufferList *output,const AudioTimeStamp*,void *ref) {
        auto &self=*static_cast<LCBridge*>(ref);
        if(output) for(unsigned b=0;b<output->mNumberBuffers;++b) if(output->mBuffers[b].mData)
            std::memset(output->mBuffers[b].mData,0,output->mBuffers[b].mDataByteSize);
        auto l=channel(const_cast<AudioBufferList*>(input),self.inputLeft),r=channel(const_cast<AudioBufferList*>(input),self.inputRight);
        auto ol=channel(output,self.outputLeft),orr=channel(output,self.outputRight);
        unsigned wanted=std::min(ol.frames,orr.frames);
        self.callbacks.fetch_add(1,std::memory_order_relaxed);
        auto min=self.minFrames.load(); if(wanted<min) self.minFrames.store(wanted);
        auto max=self.maxFrames.load(); if(wanted>max) self.maxFrames.store(wanted);
        unsigned n=l.samples&&r.samples&&ol.samples&&orr.samples?std::min({wanted,l.frames,r.frames}):0;
        self.missing.fetch_add(wanted-n,std::memory_order_relaxed);
        float peak=0;
        for(unsigned f=0;f<n;++f) {
            auto left=l.samples[f*l.stride],right=r.samples[f*r.stride];
            if(!std::isfinite(left)) left=0; if(!std::isfinite(right)) right=0;
            ol.samples[f*ol.stride]=left; orr.samples[f*orr.stride]=right;
            peak=std::max({peak,std::abs(left),std::abs(right)});
        }
        auto old=self.peak.load(std::memory_order_relaxed);
        if(peak>old) self.peak.compare_exchange_strong(old,peak,std::memory_order_relaxed);
        return noErr;
    }
    void usage(UInt32 scope,unsigned first,unsigned count) {
        auto streams=list(aggregate,kAudioDevicePropertyStreams,scope);
        const size_t bytes=offsetof(AudioHardwareIOProcStreamUsage,mStreamIsOn)+streams.size()*sizeof(UInt32);
        std::vector<unsigned char> storage(bytes); auto *u=reinterpret_cast<AudioHardwareIOProcStreamUsage*>(storage.data());
        u->mIOProc=reinterpret_cast<void*>(proc); u->mNumberStreams=UInt32(streams.size());
        for(unsigned i=0;i<streams.size();++i) u->mStreamIsOn[i]=i>=first&&i<first+count;
        AudioObjectPropertyAddress a{kAudioDevicePropertyIOProcStreamUsage,scope,kAudioObjectPropertyElementMain};
        check(AudioObjectSetPropertyData(aggregate,&a,0,nullptr,UInt32(bytes),u),"Select bridge IO streams");
    }
    void start(AudioDeviceID source,AudioDeviceID destination,unsigned bus,unsigned left,unsigned right,UInt32 frames,double rate) {
        if(aggregate||proc) throw std::runtime_error("Bridge is already running");
        if(!source||source==destination||bus>3||frames<32||frames>2048||(frames&(frames-1))||
           !(rate==44100||rate==48000||rate==88200||rate==96000)) throw std::runtime_error("Invalid bridge configuration");
        const auto sourceIns=channels(source,In),sourceOuts=channels(source,Out),destOuts=channels(destination,Out);
        if(sourceIns<bus*2+2||destOuts<=std::max(left,right)) throw std::runtime_error("Bridge channel is unavailable");
        inputLeft=bus*2; inputRight=bus*2+1; outputLeft=sourceOuts+left; outputRight=sourceOuts+right;
        auto sourceInStreams=list(source,kAudioDevicePropertyStreams,In).size();
        auto sourceOutStreams=list(source,kAudioDevicePropertyStreams,Out).size();
        auto destOutStreams=list(destination,kAudioDevicePropertyStreams,Out).size();
        requestedFrames=frames; requestedRate=rate; prepare(source); prepare(destination);
        CF sourceUID(get<CFStringRef>(source,kAudioDevicePropertyDeviceUID)),destUID(get<CFStringRef>(destination,kAudioDevicePropertyDeviceUID));
        CF uuid(CFUUIDCreate(nullptr)); CF uid(CFUUIDCreateString(nullptr,static_cast<CFUUIDRef>(uuid.ref)));
        int one=1,zero=0; CF n1(CFNumberCreate(nullptr,kCFNumberIntType,&one)),n0(CFNumberCreate(nullptr,kCFNumberIntType,&zero));
        const void *subKeys[]{CFSTR(kAudioSubDeviceUIDKey),CFSTR(kAudioSubDeviceDriftCompensationKey)};
        const void *sourceValues[]{sourceUID.ref,n1.ref},*destValues[]{destUID.ref,n0.ref};
        CF sourceDict(CFDictionaryCreate(nullptr,subKeys,sourceValues,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
        CF destDict(CFDictionaryCreate(nullptr,subKeys,destValues,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
        const void *subs[]{sourceDict.ref,destDict.ref}; CF subList(CFArrayCreate(nullptr,subs,2,&kCFTypeArrayCallBacks));
        const void *keys[]{CFSTR(kAudioAggregateDeviceUIDKey),CFSTR(kAudioAggregateDeviceNameKey),CFSTR(kAudioAggregateDeviceSubDeviceListKey),CFSTR(kAudioAggregateDeviceMainSubDeviceKey),CFSTR(kAudioAggregateDeviceIsPrivateKey)};
        const void *values[]{uid.ref,CFSTR("Patchlane output bridge"),subList.ref,destUID.ref,n1.ref};
        CF composition(CFDictionaryCreate(nullptr,keys,values,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
        check(AudioHardwareCreateAggregateDevice(static_cast<CFDictionaryRef>(composition.ref),&aggregate),"Create private bridge aggregate");
        // Creation may return before all subdevice streams become visible.
        bool ready=false;
        for(unsigned i=0;i<100;++i) {
            if(channels(aggregate,In)>=sourceIns&&channels(aggregate,Out)==sourceOuts+destOuts) { ready=true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(!ready) throw std::runtime_error("Bridge aggregate streams unavailable");
        auto subsActual=list(aggregate,kAudioAggregateDevicePropertyActiveSubDeviceList,Global);
        if(subsActual.size()!=2) throw std::runtime_error("Unexpected aggregate subdevices");
        CF actualSource(get<CFStringRef>(subsActual[0],kAudioDevicePropertyDeviceUID)),actualDest(get<CFStringRef>(subsActual[1],kAudioDevicePropertyDeviceUID));
        if(!CFEqual(actualSource.ref,sourceUID.ref)||!CFEqual(actualDest.ref,destUID.ref)) throw std::runtime_error("Unexpected aggregate channel order");
        setTiming(aggregate,kAudioDevicePropertyNominalSampleRate,rate);
        setTiming(aggregate,kAudioDevicePropertyBufferFrameSize,frames);
        check(AudioDeviceCreateIOProcID(aggregate,callback,this,&proc),"Create bridge callback");
        // Do not enable the physical microphone or loop back into the virtual output.
        usage(In,0,unsigned(sourceInStreams)); usage(Out,unsigned(sourceOutStreams),unsigned(destOutStreams));
        callbacks=0; missing=0; minFrames=UINT32_MAX; maxFrames=0; peak=0;
        check(AudioDeviceStart(aggregate,proc),"Start bridge");
    }
};
// Direct shared-ring reader. The virtual capture callback and aggregate device
// are absent from this path; only the physical output schedules consumption.
struct LCSharedOutput {
    bool stereoSource=false;
    int sourceLeft=-1,sourceRight=-1;
    std::atomic<float> inputGain{1},routeGain{1},inputPeak{0},outputPeak{0};
    float currentInput=1,currentRoute=1;
    static void meter(std::atomic<float> &value,float peak) {
        auto old=value.load(std::memory_order_relaxed);
        if(peak>old)value.compare_exchange_strong(old,peak,std::memory_order_relaxed);
    }
    lcshared::InterpolationKernel interpolation;
    std::atomic<bool> needsReconnect{false};
    lcshared::MappedAudioRing mapping;
    LCBridge timing,sourceTiming;
    AudioDeviceID device=0;
    AudioDeviceIOProcID proc=nullptr;
    unsigned bus=0,left=0,right=1;
    double nominalTicks=0,delay=0;
    char error[256]{};
    std::atomic<uint64_t> callbacks{0},missing{0};
    std::atomic<uint64_t> invalidClock{0};
    std::atomic<uint64_t> initialMissing{0},received{0};
    bool hasReceived=false; // Owned by this IOProc; reset only while stopped.
    void stop() {
        if(proc) { AudioDeviceStop(device,proc);AudioDeviceDestroyIOProcID(device,proc);proc=nullptr; }
        device=0;mapping.close();timing.stop();sourceTiming.stop();
    }
    ~LCSharedOutput() { stop(); }
    void observeSource(const lcshared::MappedAudioRing &candidate) {
        if(candidate.view()&&mapping.view()&&!mapping.sameEntry(candidate))
            needsReconnect.store(true,std::memory_order_relaxed);
    }
    static OSStatus callback(AudioDeviceID,const AudioTimeStamp*,const AudioBufferList*,const AudioTimeStamp*,
                             AudioBufferList *output,const AudioTimeStamp *when,void *context) {
        auto &self=*static_cast<LCSharedOutput*>(context);
        if(output) for(unsigned b=0;b<output->mNumberBuffers;++b) if(output->mBuffers[b].mData)
            std::memset(output->mBuffers[b].mData,0,output->mBuffers[b].mDataByteSize);
        const auto l=channel(output,self.left),r=channel(output,self.right);
        const auto frames=std::min(l.frames,r.frames);
        self.callbacks.fetch_add(1,std::memory_order_relaxed);
        // RateScalar is actual ticks/frame divided by nominal ticks/frame.
        // Refuse missing clock information instead of silently assuming sync.
        if(!l.samples||!r.samples||!when||
           (when->mFlags&(kAudioTimeStampHostTimeValid|kAudioTimeStampRateScalarValid))!=
            (kAudioTimeStampHostTimeValid|kAudioTimeStampRateScalarValid)||
           !std::isfinite(when->mRateScalar)||when->mRateScalar<=0) {
            self.invalidClock.fetch_add(1,std::memory_order_relaxed);
            if(!self.hasReceived) self.initialMissing.fetch_add(frames,std::memory_order_relaxed);
            self.missing.fetch_add(frames,std::memory_order_relaxed);return noErr;
        }
        lcshared::TimedReader reader(*self.mapping.view(),when->mHostTime,
                                    self.nominalTicks*when->mRateScalar,self.delay,&self.interpolation);
        if(reader.rateChanged()) self.needsReconnect.store(true,std::memory_order_relaxed);
        uint64_t lost=0,initial=0,read=0;
        float inPeak=0,outPeak=0;
        const float inputTarget=self.inputGain.load(std::memory_order_relaxed);
        const float routeTarget=self.routeGain.load(std::memory_order_relaxed);
        const float slew=1.f-std::exp(-1.f/(float(self.timing.requestedRate)*0.005f));
        for(unsigned f=0;f<frames;++f) {
            float a=0,b=0;
            bool valid=false;
            if(self.sourceLeft<0) valid=reader.read(f,self.bus,a,b);
            else {
                float x=0,y=0;
                valid=reader.read(f,unsigned(self.sourceLeft)/2,x,y);
                a=(self.sourceLeft&1)?y:x;
                if(self.sourceRight/2==self.sourceLeft/2) b=(self.sourceRight&1)?y:x;
                else { valid=reader.read(f,unsigned(self.sourceRight)/2,x,y)&&valid;b=(self.sourceRight&1)?y:x; }
            }
            if(!valid) { a=b=0;++lost;if(!self.hasReceived) ++initial; }
            else { self.hasReceived=true;++read; }
            self.currentInput+=(inputTarget-self.currentInput)*slew;
            self.currentRoute+=(routeTarget-self.currentRoute)*slew;
            a*=self.currentInput;b*=self.currentInput;
            inPeak=std::max(inPeak,std::max(std::abs(a),std::abs(b)));
            a*=self.currentRoute;b*=self.currentRoute;
            outPeak=std::max(outPeak,std::max(std::abs(a),std::abs(b)));
            l.samples[f*l.stride]=std::clamp(a,-1.f,1.f);r.samples[f*r.stride]=std::clamp(b,-1.f,1.f);
        }
        meter(self.inputPeak,inPeak);meter(self.outputPeak,outPeak);
        self.initialMissing.fetch_add(initial,std::memory_order_relaxed);
        self.received.fetch_add(read,std::memory_order_relaxed);
        self.missing.fetch_add(lost,std::memory_order_relaxed);return noErr;
    }
    void start(AudioDeviceID destination,int sourceBus,int outputLeft,int outputRight,int frames,double rate,double delayFrames,bool stereo=false) {
        stereoSource=stereo;
        if(stereo&&sourceBus!=0) throw std::runtime_error("Stereo source only has Main bus");
        const auto sourceUID=stereo?CFSTR("audio.patchlane.virtual2"):CFSTR("audio.patchlane.virtual8");
        if(proc) throw std::runtime_error("Shared output is already running");
        if(!destination||sourceBus<0||sourceBus>3||outputLeft<0||outputRight<0||outputLeft==outputRight||
           frames<32||frames>2048||(frames&(frames-1))||
           !(rate==44100||rate==48000||rate==88200||rate==96000)||
           !std::isfinite(delayFrames)||delayFrames<0||delayFrames>=lcshared::Capacity)
            throw std::runtime_error("Invalid shared output configuration");
        CF uid(get<CFStringRef>(destination,kAudioDevicePropertyDeviceUID));
        if(CFEqual(uid.ref,sourceUID)) throw std::runtime_error("Shared output cannot feed its own virtual device");
        const auto status=lcshared::brokerExchange(mapping,false,lcshared::BrokerExecutable,0,nullptr,3000,stereo);
        if(status!=KERN_SUCCESS) throw std::runtime_error("Acquire shared ring: "+std::to_string(status));
        double sourceRate=0;
        for(auto candidate:list(kAudioObjectSystemObject,kAudioHardwarePropertyDevices,Global)) {
            CF candidateUID(get<CFStringRef>(candidate,kAudioDevicePropertyDeviceUID));
            if(CFEqual(candidateUID.ref,sourceUID)) {
                sourceRate=get<double>(candidate,kAudioDevicePropertyNominalSampleRate);break;
            }
        }
        if(!std::isfinite(sourceRate)||sourceRate<44100||sourceRate>96000)
            throw std::runtime_error("Cannot determine shared source sample rate");
        interpolation.prepare(sourceRate/rate); // No table generation in the IOProc.
        device=destination;bus=unsigned(sourceBus);left=unsigned(outputLeft);right=unsigned(outputRight);delay=delayFrames;
        timing.requestedFrames=uint32_t(frames);timing.requestedRate=rate;timing.prepare(device);
        if(channels(device,Out)<=std::max(left,right)) throw std::runtime_error("Shared output channel unavailable");
        mach_timebase_info_data_t timebase{};check(mach_timebase_info(&timebase),"Read host clock");
        nominalTicks=1e9*double(timebase.denom)/double(timebase.numer)/rate;
        check(AudioDeviceCreateIOProcID(device,callback,this,&proc),"Create shared output callback");
        const auto inputs=list(device,kAudioDevicePropertyStreams,In);
        if(!inputs.empty()) {
            const size_t bytes=offsetof(AudioHardwareIOProcStreamUsage,mStreamIsOn)+inputs.size()*sizeof(UInt32);
            std::vector<unsigned char> storage(bytes);
            auto *usage=reinterpret_cast<AudioHardwareIOProcStreamUsage*>(storage.data());
            usage->mIOProc=reinterpret_cast<void*>(proc);usage->mNumberStreams=UInt32(inputs.size());
            AudioObjectPropertyAddress address{kAudioDevicePropertyIOProcStreamUsage,In,kAudioObjectPropertyElementMain};
            check(AudioObjectSetPropertyData(device,&address,0,nullptr,UInt32(bytes),usage),"Disable physical input streams");
        }
        callbacks=0;missing=0;invalidClock=0;initialMissing=0;received=0;hasReceived=false;needsReconnect=false;
        inputPeak=0;outputPeak=0;currentInput=inputGain.load();currentRoute=routeGain.load();
        check(AudioDeviceStart(device,proc),"Start shared output");
    }
};
extern "C" {
int lc_shared_silent_check(uint32_t source,uint32_t destination,int frames,double rate,double delayFrames,double seconds,char *report,int capacity) {
    LCBridge sourceTiming;
    LCSharedOutput output;
    AudioDeviceIOProcID sourceProc=nullptr;
    auto cleanup=[&] {
        output.stop();
        if(sourceProc) { AudioDeviceStop(source,sourceProc);AudioDeviceDestroyIOProcID(source,sourceProc);sourceProc=nullptr; }
        sourceTiming.stop();
    };
    try {
        if(!std::isfinite(seconds)||seconds<1||seconds>600||!source||!destination||source==destination||frames<32||frames>2048||(frames&(frames-1))||
           !(rate==44100||rate==48000||rate==88200||rate==96000)) throw std::runtime_error("Invalid silent check configuration");
        CF uid(get<CFStringRef>(source,kAudioDevicePropertyDeviceUID));
        if(!CFEqual(uid.ref,CFSTR("audio.patchlane.virtual8"))) throw std::runtime_error("Silent check requires dedicated virtual source");
        if(get<UInt32>(source,kAudioDevicePropertyDeviceIsRunningSomewhere)||get<UInt32>(destination,kAudioDevicePropertyDeviceIsRunningSomewhere))
            throw std::runtime_error("Device is in use; left unchanged");
        const auto oldSourceFrames=get<UInt32>(source,kAudioDevicePropertyBufferFrameSize);
        const auto oldDestFrames=get<UInt32>(destination,kAudioDevicePropertyBufferFrameSize);
        const auto oldSourceRate=get<double>(source,kAudioDevicePropertyNominalSampleRate);
        const auto oldDestRate=get<double>(destination,kAudioDevicePropertyNominalSampleRate);
        sourceTiming.requestedFrames=UInt32(frames);sourceTiming.requestedRate=rate;sourceTiming.prepare(source);
        auto silence=+[](AudioDeviceID,const AudioTimeStamp*,const AudioBufferList*,const AudioTimeStamp*,AudioBufferList *buffers,const AudioTimeStamp*,void*)->OSStatus {
            if(buffers) for(UInt32 b=0;b<buffers->mNumberBuffers;++b)
                if(buffers->mBuffers[b].mData) std::memset(buffers->mBuffers[b].mData,0,buffers->mBuffers[b].mDataByteSize);
            return noErr;
        };
        check(AudioDeviceCreateIOProcID(source,silence,nullptr,&sourceProc),"Create silent virtual source");
        check(AudioDeviceStart(source,sourceProc),"Start silent virtual source");
        output.start(destination,0,0,1,frames,rate,delayFrames);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto before=output.missing.load();
        const auto callbacksBefore=output.callbacks.load(),receivedBefore=output.received.load();
        bool sourceStable=lc_shared_output_refresh_source(&output)==KERN_SUCCESS&&!output.needsReconnect.load();
        const auto begin=std::chrono::steady_clock::now();
        const auto deadline=begin+std::chrono::duration<double>(seconds);
        auto nextSourceCheck=begin+std::chrono::seconds(10);
        auto nextProgress=begin+std::chrono::seconds(30);
        auto previousCallbacks=output.callbacks.load();
        auto previousCheck=begin;
        while(std::chrono::steady_clock::now()<deadline) {
            std::this_thread::sleep_for(std::min<std::chrono::duration<double>>(std::chrono::duration<double>(1),deadline-std::chrono::steady_clock::now()));
            if(get<double>(source,kAudioDevicePropertyNominalSampleRate)!=rate ||
               get<double>(destination,kAudioDevicePropertyNominalSampleRate)!=rate ||
               get<UInt32>(source,kAudioDevicePropertyBufferFrameSize)!=UInt32(frames) ||
               get<UInt32>(destination,kAudioDevicePropertyBufferFrameSize)!=UInt32(frames))
                throw std::runtime_error("Device timing changed; silent check stopped");
            const auto now=std::chrono::steady_clock::now();
            if(now>=nextSourceCheck) {
                sourceStable=(lc_shared_output_refresh_source(&output)==KERN_SUCCESS)&&sourceStable;
                nextSourceCheck=now+std::chrono::seconds(10);
            }
            if(output.needsReconnect.load()) throw std::runtime_error("Source clock or mapping changed; silent check stopped");
            const auto currentCallbacks=output.callbacks.load();
            if(now-previousCheck>=std::chrono::milliseconds(500)&&currentCallbacks==previousCallbacks) throw std::runtime_error("Output callbacks stalled; silent check stopped");
            previousCallbacks=currentCallbacks;previousCheck=now;
            if(seconds>60&&now>=nextProgress) {
                std::printf("progress seconds=%.0f steadyMissing=%llu received=%llu sourceStable=%d\n",
                    std::chrono::duration<double>(now-begin).count(),
                    (unsigned long long)(output.missing.load()-before),(unsigned long long)output.received.load(),sourceStable);
                std::fflush(stdout);nextProgress=now+std::chrono::seconds(30);
            }
        }
        const auto steady=output.missing.load()-before;
        cleanup();
        const bool restored=get<UInt32>(source,kAudioDevicePropertyBufferFrameSize)==oldSourceFrames&&
            get<UInt32>(destination,kAudioDevicePropertyBufferFrameSize)==oldDestFrames&&
            get<double>(source,kAudioDevicePropertyNominalSampleRate)==oldSourceRate&&
            get<double>(destination,kAudioDevicePropertyNominalSampleRate)==oldDestRate;
        const bool ok=output.callbacks>callbacksBefore&&output.received>receivedBefore&&output.invalidClock==0&&steady==0&&restored&&sourceStable;
        if(report&&capacity>0) std::snprintf(report,size_t(capacity),
            "%s seconds=%.1f frames=%d rate=%.0f delay=%.1f callbacks=%llu received=%llu steadyMissing=%llu totalMissing=%llu invalidClock=%llu restored=%d sourceStable=%d",
            ok?"PASS":"FAIL",seconds,frames,rate,delayFrames,(unsigned long long)output.callbacks.load(),
            (unsigned long long)output.received.load(),(unsigned long long)steady,(unsigned long long)output.missing.load(),
            (unsigned long long)output.invalidClock.load(),restored,sourceStable);
        return ok?0:1;
    } catch(const std::exception &e) {
        cleanup();if(report&&capacity>0) std::snprintf(report,size_t(capacity),"ERROR: %s",e.what());return 1;
    }
}
LCSharedOutput *lc_shared_output_create() { return new LCSharedOutput; }
void lc_shared_output_destroy(LCSharedOutput *output) { delete output; }
int lc_shared_output_start(LCSharedOutput *output,uint32_t device,int bus,int left,int right,int frames,double rate,double delayFrames) {
    if(!output) return -1;
    if(output->proc) { std::snprintf(output->error,sizeof(output->error),"Shared output is already running");return -1; }
    output->error[0]=0;
    try { output->start(device,bus,left,right,frames,rate,delayFrames);return 0; }
    catch(const std::exception &e) { std::snprintf(output->error,sizeof(output->error),"%s",e.what());output->stop();return -1; }
}
int lc_shared_output_start_source(LCSharedOutput *output,uint32_t source,uint32_t device,int bus,int left,int right,int frames,double rate,double delayFrames) {
    if(!output) return -1;
    if(output->proc) return -1;
    output->error[0]=0;
    try {
        CF uid(get<CFStringRef>(source,kAudioDevicePropertyDeviceUID));
        const bool stereo=CFEqual(uid.ref,CFSTR("audio.patchlane.virtual2"));
        if(!stereo&&!CFEqual(uid.ref,CFSTR("audio.patchlane.virtual8"))) throw std::runtime_error("Unsupported dedicated source");
        if(stereo&&(output->sourceLeft>=2||output->sourceRight>=2))throw std::runtime_error("Stereo source channel unavailable");
        output->sourceTiming.requestedFrames=uint32_t(frames);output->sourceTiming.requestedRate=rate;
        output->sourceTiming.prepare(source);
        output->start(device,bus,left,right,frames,rate,delayFrames,stereo);return 0;
    } catch(const std::exception &e) { std::snprintf(output->error,sizeof(output->error),"%s",e.what());output->stop();return -1; }
}
int lc_shared_output_channels(LCSharedOutput *output,int left,int right) {
    if(!output||output->proc||left<0||right<0||left>=8||right>=8)return -1;
    output->sourceLeft=left;output->sourceRight=right;return 0;
}
void lc_shared_output_levels(LCSharedOutput *output,float gain,int routed,int muted) {
    if(!output)return;
    output->inputGain.store(std::isfinite(gain)?std::clamp(gain,0.f,64.f):0.f,std::memory_order_relaxed);
    output->routeGain.store(routed&&!muted?1.f:0.f,std::memory_order_relaxed);
}
float lc_shared_output_input_peak(LCSharedOutput *output) { return output?output->inputPeak.exchange(0):0; }
float lc_shared_output_peak(LCSharedOutput *output) { return output?output->outputPeak.exchange(0):0; }
void lc_shared_output_stop(LCSharedOutput *output) { if(output) output->stop(); }
const char *lc_shared_output_error(LCSharedOutput *output) { return output?output->error:"Invalid shared output"; }
uint64_t lc_shared_output_callbacks(LCSharedOutput *output) { return output?output->callbacks.load():0; }
uint64_t lc_shared_output_missing_frames(LCSharedOutput *output) { return output?output->missing.load():0; }
uint64_t lc_shared_output_invalid_clock(LCSharedOutput *output) { return output?output->invalidClock.load():0; }
uint64_t lc_shared_output_initial_missing_frames(LCSharedOutput *output) { return output?output->initialMissing.load():0; }
uint64_t lc_shared_output_received_frames(LCSharedOutput *output) { return output?output->received.load():0; }
LCBridge *lc_bridge_create() { return new LCBridge; }
void lc_bridge_destroy(LCBridge *b) { delete b; }
int lc_bridge_start(LCBridge *b,uint32_t source,uint32_t destination,int bus,int left,int right,int frames,double rate) {
    if(!b) return -1; b->error[0]=0;
    if(b->aggregate||b->proc) { std::snprintf(b->error,sizeof(b->error),"Bridge is already running"); return -1; }
    try { if(bus<0||left<0||right<0||frames<0) throw std::runtime_error("Invalid bridge channel or timing"); b->start(source,destination,bus,left,right,frames,rate); return 0; }
    catch(const std::exception &e) { std::snprintf(b->error,sizeof(b->error),"%s",e.what()); b->stop(); return -1; }
}
void lc_bridge_stop(LCBridge *b) { if(b) b->stop(); }
const char *lc_bridge_error(LCBridge *b) { return b?b->error:"Invalid bridge"; }
uint64_t lc_bridge_callbacks(LCBridge *b) { return b?b->callbacks.load():0; }
uint64_t lc_bridge_missing_frames(LCBridge *b) { return b?b->missing.load():0; }
uint32_t lc_bridge_min_frames(LCBridge *b) { return b?b->minFrames.load():0; }
uint32_t lc_bridge_max_frames(LCBridge *b) { return b?b->maxFrames.load():0; }
float lc_bridge_peak(LCBridge *b) { return b?b->peak.exchange(0):0; }
}

int lc_shared_output_needs_reconnect(LCSharedOutput *output) { return output&&output->needsReconnect.load(std::memory_order_relaxed); }

int lc_shared_output_refresh_source(LCSharedOutput *output) {
    if(!output||!output->proc) return KERN_FAILURE;
    lcshared::MappedAudioRing candidate;
    const auto status=lcshared::brokerExchange(candidate,false,lcshared::BrokerExecutable,0,nullptr,100,output->stereoSource);
    if(status==KERN_SUCCESS) output->observeSource(candidate);
    return status;
}
