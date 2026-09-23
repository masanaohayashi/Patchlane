// Synthetic, virtual-device-only latency comparison. Never records a microphone.
// Usage: loopback-latency sourceUID destinationUID frames rate seconds
// Same UID = direct loopback; different UIDs = through current AudioCore mixer.
#include "AudioCore.h"
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <chrono>
#include <string>
#include <stdexcept>
#include <memory>

namespace {
constexpr unsigned Limit=32768,Interval=1024;
struct Marker { double sampleHost=0,callbackHost=0; bool present=false; };
struct Context {
    bool generate=false,receive=false,previousPulse=false,badLayout=false;
    uint64_t frames=0; unsigned callbacks=0,minFrames=UINT32_MAX,maxFrames=0,duplicates=0,invalid=0;
    double ticksPerFrame=0;
    float pulsePeak=0; double pulseCode=0; Marker pulseMarker;
    std::array<Marker,Limit> markers{};
};
float *channel(AudioBufferList *list,unsigned ch,unsigned frame) {
    if(!list) return nullptr;
    for(unsigned b=0;b<list->mNumberBuffers;++b) {
        auto &buf=list->mBuffers[b];
        if(ch<buf.mNumberChannels) {
            auto offset=frame*buf.mNumberChannels+ch;
            if(!buf.mData||(offset+1)*sizeof(float)>buf.mDataByteSize) return nullptr;
            return static_cast<float*>(buf.mData)+offset;
        }
        ch-=buf.mNumberChannels;
    }
    return nullptr;
}
unsigned count(const AudioBufferList *list) {
    if(!list||!list->mNumberBuffers||!list->mBuffers[0].mNumberChannels) return 0;
    return list->mBuffers[0].mDataByteSize/(sizeof(float)*list->mBuffers[0].mNumberChannels);
}
OSStatus callback(AudioDeviceID,const AudioTimeStamp*,const AudioBufferList *input,const AudioTimeStamp *inputTime,
                  AudioBufferList *output,const AudioTimeStamp *outputTime,void *ref) {
    auto &c=*static_cast<Context*>(ref);
    const auto now=double(mach_absolute_time());
    if(output) for(unsigned b=0;b<output->mNumberBuffers;++b) if(output->mBuffers[b].mData)
        std::memset(output->mBuffers[b].mData,0,output->mBuffers[b].mDataByteSize);
    auto n=c.generate?count(output):count(input);
    ++c.callbacks; c.minFrames=std::min(c.minFrames,n); c.maxFrames=std::max(c.maxFrames,n);
    if(c.generate) {
        if(!outputTime||(outputTime->mFlags&kAudioTimeStampHostTimeValid)==0) { c.badLayout=true; return noErr; }
        for(unsigned f=0;f<n;++f) {
            auto index=c.frames+f;
            const auto phase=index%Interval;
            if(phase>=8) continue;
            auto id=index/Interval; if(id>=Limit) continue;
            auto l=channel(output,0,f),r=channel(output,1,f);
            if(!l||!r) { c.badLayout=true; continue; }
            // Peak timing is independent of constant device attenuation.
            float amplitude=0.125f*float(std::pow(std::sin(3.141592653589793*double(phase)/8),2));
            *l=amplitude; *r=amplitude*float(id+1)/float(Limit);
            if(phase==4) c.markers[id]={double(outputTime->mHostTime)+f*c.ticksPerFrame,now,true};
        }
        c.frames+=n;
    }
    if(c.receive) {
        if(!inputTime||(inputTime->mFlags&kAudioTimeStampHostTimeValid)==0) { c.badLayout=true; return noErr; }
        // For duplex direct loopback use a separate receiver Context (below).
        for(unsigned f=0;f<count(input);++f) {
            auto l=channel(const_cast<AudioBufferList*>(input),0,f),r=channel(const_cast<AudioBufferList*>(input),1,f);
            if(!l||!r) { c.badLayout=true; break; }
            const bool pulse=std::isfinite(*l)&&std::isfinite(*r)&&std::abs(*l)>0.001;
            if(pulse && std::abs(*l)>c.pulsePeak) {
                c.pulsePeak=std::abs(*l);
                c.pulseCode=double(*r)/double(*l)*Limit;
                c.pulseMarker={double(inputTime->mHostTime)+f*c.ticksPerFrame,now,true};
            }
            if(!pulse && c.previousPulse) {
                const auto code=c.pulseCode;
                if(!std::isfinite(code)||code<0.5||code>Limit+0.5) ++c.invalid;
                else {
                    const auto id=std::llround(code)-1;
                    if(id<0||id>=Limit||std::abs(code-double(id+1))>0.1) ++c.invalid;
                    else if(c.markers[id].present) ++c.duplicates;
                    else c.markers[id]=c.pulseMarker;
                }
                c.pulsePeak=0;
            }
            c.previousPulse=pulse;
        }
    }
    return noErr;
}
struct Duplex { Context send,receive; };
OSStatus duplex(AudioDeviceID d,const AudioTimeStamp *now,const AudioBufferList *in,const AudioTimeStamp *it,AudioBufferList *out,const AudioTimeStamp *ot,void *ref) {
    auto &c=*static_cast<Duplex*>(ref);
    // Receive first, so rendering cannot erase aliased host input storage.
    callback(d,now,in,it,nullptr,ot,&c.receive);
    return callback(d,now,nullptr,it,out,ot,&c.send);
}
template<class T> T read(AudioDeviceID id,UInt32 selector,UInt32 scope=kAudioObjectPropertyScopeGlobal) {
    T value{}; UInt32 size=sizeof(value);
    AudioObjectPropertyAddress a{selector,scope,kAudioObjectPropertyElementMain};
    auto e=AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&value);
    if(e||size!=sizeof(value)) throw std::runtime_error("HAL property read failed: "+std::to_string(e));
    return value;
}
template<class T> void set(AudioDeviceID id,UInt32 selector,T value) {
    AudioObjectPropertyAddress a{selector,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    auto e=AudioObjectSetPropertyData(id,&a,0,nullptr,sizeof(value),&value);
    if(e) throw std::runtime_error("HAL property write failed: "+std::to_string(e));
    for(int i=0;i<1000;++i) {
        if(read<T>(id,selector)==value) return;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.005,false);
    }
    throw std::runtime_error("Device did not accept requested timing: selector="+std::to_string(selector)+" requested="+std::to_string(value)+" actual="+std::to_string(read<T>(id,selector)));
}
struct DeviceSession {
    AudioDeviceID device=0; AudioDeviceIOProcID proc=nullptr;
    UInt32 originalFrames=0,targetFrames=0; double originalRate=0,targetRate=0; bool saved=false;
    void prepare(AudioDeviceID id,UInt32 frames,double rate) {
        device=id;
        if(read<UInt32>(id,kAudioDevicePropertyDeviceIsRunningSomewhere)) throw std::runtime_error("Device is in use; left unchanged");
        // IOProc client buffers must be float PCM; don't reinterpret arbitrary hardware formats.
        for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}) {
            AudioObjectPropertyAddress a{kAudioDevicePropertyStreams,scope,kAudioObjectPropertyElementMain};
            UInt32 bytes=0; if(AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&bytes)||!bytes) throw std::runtime_error("Missing virtual IO stream");
            std::vector<AudioStreamID> streams(bytes/sizeof(AudioStreamID));
            if(AudioObjectGetPropertyData(id,&a,0,nullptr,&bytes,streams.data())) throw std::runtime_error("Cannot inspect streams");
            for(auto stream:streams) {
                auto f=read<AudioStreamBasicDescription>(stream,kAudioStreamPropertyVirtualFormat);
                if(f.mFormatID!=kAudioFormatLinearPCM||!(f.mFormatFlags&kAudioFormatFlagIsFloat)||f.mBitsPerChannel!=32)
                    throw std::runtime_error("Benchmark requires Float32 client buffers");
            }
        }
        originalFrames=read<UInt32>(id,kAudioDevicePropertyBufferFrameSize);
        originalRate=read<double>(id,kAudioDevicePropertyNominalSampleRate); saved=true;
        targetFrames=frames; targetRate=rate;
        if(originalRate!=rate) set(id,kAudioDevicePropertyNominalSampleRate,rate);
        if(originalFrames!=frames) set(id,kAudioDevicePropertyBufferFrameSize,frames);
    }
    void start(AudioDeviceIOProc fn,void *context) {
        if(AudioDeviceCreateIOProcID(device,fn,context,&proc)||AudioDeviceStart(device,proc)) throw std::runtime_error("IOProc start failed");
    }
    void stop() { if(proc) { AudioDeviceStop(device,proc); AudioDeviceDestroyIOProcID(device,proc); proc=nullptr; } }
    ~DeviceSession() {
        stop(); if(!saved) return;
        try {
            if(read<UInt32>(device,kAudioDevicePropertyBufferFrameSize)==targetFrames) set(device,kAudioDevicePropertyBufferFrameSize,originalFrames);
            if(read<double>(device,kAudioDevicePropertyNominalSampleRate)==targetRate) set(device,kAudioDevicePropertyNominalSampleRate,originalRate);
        }
        catch(const std::exception &e) { std::fprintf(stderr,"RESTORE FAILED: %s\n",e.what()); }
    }
};
void report(const char *name,std::vector<double> values) {
    std::sort(values.begin(),values.end());
    auto percentile=[&](double p) { return values[size_t(std::ceil(p*(values.size()-1)))]; };
    std::printf("%s_ms median=%.6f p99=%.6f min=%.6f max=%.6f\n",name,percentile(.5),percentile(.99),values.front(),values.back());
}
}
// Exercise the actual generator/detector with attenuation and delays crossing
// callback boundaries. This guards the latency measurement itself.
static int detectorSelfTest() {
    for(unsigned firstID:{0u,Limit-8}) for(float gain:{0.1f,0.1875f,1.f}) for(unsigned delay:{0u,7u,31u,65u}) {
        Context send,receive; send.generate=true; receive.receive=true;
        send.frames=uint64_t(firstID)*Interval;
        send.ticksPerFrame=receive.ticksPerFrame=1;
        std::vector<float> history(8192*2,0);
        for(unsigned start=0;start<8192;start+=32) {
            float output[64]{},input[64]{};
            AudioBufferList out{1,{{2,sizeof(output),output}}};
            AudioBufferList in{1,{{2,sizeof(input),input}}};
            AudioTimeStamp stamp{};stamp.mFlags=kAudioTimeStampHostTimeValid;stamp.mHostTime=100000+start;
            callback(0,nullptr,nullptr,nullptr,&out,&stamp,&send);
            std::copy(output,output+64,history.begin()+start*2);
            for(unsigned frame=0;frame<32;++frame) if(start+frame>=delay)
                for(unsigned ch=0;ch<2;++ch) input[frame*2+ch]=gain*history[(start+frame-delay)*2+ch];
            callback(0,nullptr,&in,&stamp,nullptr,nullptr,&receive);
        }
        for(unsigned id=firstID;id<firstID+7;++id) {
            if(!send.markers[id].present||!receive.markers[id].present||
               receive.markers[id].sampleHost-send.markers[id].sampleHost!=delay||receive.invalid||receive.duplicates) {
                std::fprintf(stderr,"FAIL detector gain=%g delay=%u id=%u\n",gain,delay,id); return 1;
            }
        }
    }
    std::puts("PASS peak detector: attenuation, known sample delay, callback boundaries");return 0;
}

#include "physical-latency.hpp"

int runLoopbackLatency(int argc,char **argv) {
    if(argc>=2 && std::strcmp(argv[1],"--physical")==0) return runPhysicalLatency(argc,argv);
    if(argc==2 && std::strcmp(argv[1],"--selftest")==0) return detectorSelfTest();
    if(argc!=6&&argc!=7&&argc!=8) { std::fprintf(stderr,"Usage: %s sourceUID destinationUID frames rate seconds [aggregate | shared delayFrames]\n",argv[0]); return 2; }
    try {
        bool aggregate=argc==7&&std::strcmp(argv[6],"aggregate")==0;
        bool shared=argc==8&&std::strcmp(argv[6],"shared")==0;
        if((argc==7&&!aggregate)||(argc==8&&!shared)) throw std::runtime_error("Unknown bridge mode");
        const double delay=shared?std::stod(argv[7]):0;
        if(!std::isfinite(delay)||delay<0||delay>=65536) throw std::runtime_error("Invalid shared delay");
        if(shared&&(std::strcmp(argv[1],"audio.patchlane.virtual8")!=0||std::strcmp(argv[1],argv[2])==0))
            throw std::runtime_error("Shared test requires dedicated source and a separate virtual destination");
        auto frames=unsigned(std::stoul(argv[3])); auto rate=std::stod(argv[4]); auto seconds=std::stod(argv[5]);
        if((frames!=32&&frames!=64&&frames!=128)||!(rate==44100||rate==48000)||!std::isfinite(seconds)||seconds<2||seconds>600) throw std::runtime_error("Use 32/64/128 frames, 44100/48000 Hz and 2..600 seconds");
        std::vector<LCDevice> devices(lc_devices(nullptr,0)+16); auto count=lc_devices(devices.data(),int(devices.size()));
        auto find=[&](const char *uid) {
            for(int i=0;i<count&&i<int(devices.size());++i) if(std::strcmp(devices[i].uid,uid)==0) {
                auto &d=devices[i];
                if(d.inputs<2||d.outputs<2||!(std::strstr(d.name,"BlackHole")||std::strstr(d.name,"Patchlane Virtual")||std::strstr(d.name,"Pro Tools Audio Bridge")))
                    throw std::runtime_error("Only known virtual loopback devices may be tested");
                return d.id;
            }
            throw std::runtime_error(std::string("Virtual device not found: ")+uid);
        };
        auto source=find(argv[1]),dest=find(argv[2]);
        // Callback storage must outlive IOProc teardown on every exception path.
        auto storage=std::make_unique<Duplex>();
        auto &contexts=*storage;
        DeviceSession sourceSession,destinationSession;
        sourceSession.prepare(source,frames,rate);
        if(dest!=source) destinationSession.prepare(dest,frames,rate);
        mach_timebase_info_data_t timebase{}; mach_timebase_info(&timebase);
        const double ticksPerSecond=1e9*double(timebase.denom)/timebase.numer;
        contexts.send.generate=true; contexts.receive.receive=true;
        contexts.send.ticksPerFrame=contexts.receive.ticksPerFrame=ticksPerSecond/rate;
        // Declared after device sessions so engine cleanup precedes timing restoration.
        auto engine=std::unique_ptr<LCEngine,decltype(&lc_destroy)>(lc_create(),lc_destroy);
        auto bridge=std::unique_ptr<LCBridge,decltype(&lc_bridge_destroy)>(lc_bridge_create(),lc_bridge_destroy);
        auto sharedOutput=std::unique_ptr<LCSharedOutput,decltype(&lc_shared_output_destroy)>(lc_shared_output_create(),lc_shared_output_destroy);
        if(dest!=source) {
            if(shared) {
                if(lc_shared_output_start(sharedOutput.get(),dest,0,0,1,int(frames),rate,delay)) throw std::runtime_error(lc_shared_output_error(sharedOutput.get()));
            } else if(aggregate) {
                if(lc_bridge_start(bridge.get(),source,dest,0,0,1,int(frames),rate)) throw std::runtime_error(lc_bridge_error(bridge.get()));
            } else {
                lc_config_timing(engine.get(),int(frames),rate); lc_config_input(engine.get(),0,source,0,1);
                lc_config_output(engine.get(),0,dest); lc_route(engine.get(),0,0,1);
                if(lc_start(engine.get())) throw std::runtime_error(lc_error(engine.get()));
            }
            destinationSession.start(callback,&contexts.receive);
            sourceSession.start(callback,&contexts.send);
        } else sourceSession.start(duplex,&contexts);
        // Count every missing frame in the steady interval, including the
        // silence between markers. Marker matching alone can miss dropouts.
        std::this_thread::sleep_for(std::chrono::duration<double>(0.5));
        const auto missingBefore=lc_shared_output_missing_frames(sharedOutput.get());
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds-0.75));
        const auto missingAfter=lc_shared_output_missing_frames(sharedOutput.get());
        const auto steadyMissing=missingAfter-missingBefore;
        std::this_thread::sleep_for(std::chrono::duration<double>(0.25));
        sourceSession.stop(); destinationSession.stop(); lc_stop(engine.get()); lc_bridge_stop(bridge.get());lc_shared_output_stop(sharedOutput.get());
        auto &send=contexts.send; auto &receive=contexts.receive;
        std::vector<double> scheduled,age;
        unsigned expected=0,lost=0;
        // Ignore startup gain transitions and the incomplete last 0.25s.
        for(unsigned id=0;id<Limit;++id) {
            double t=double(id*Interval)/rate;
            if(t<0.5||t>seconds-0.25||!send.markers[id].present) continue;
            ++expected;
            if(!receive.markers[id].present) { ++lost; continue; }
            auto &s=send.markers[id]; auto &r=receive.markers[id];
            scheduled.push_back((r.sampleHost-s.sampleHost)*1000/ticksPerSecond);
            age.push_back((r.callbackHost-s.callbackHost)*1000/ticksPerSecond);
        }
        std::printf("path=%s frames=%u rate=%.0f matched=%zu expected=%u missing=%u duplicates=%u invalid=%u\n",dest==source?"direct":shared?"shared ring":aggregate?"aggregate bridge":"AudioCore mixer",frames,rate,scheduled.size(),expected,lost,receive.duplicates,receive.invalid);
        if(shared) std::printf("shared_delay_frames=%.3f shared_callbacks=%llu invalid_clock=%llu missing_shared_frames_including_startup=%llu steady_missing_frames=%llu\n",delay,
            (unsigned long long)lc_shared_output_callbacks(sharedOutput.get()),(unsigned long long)lc_shared_output_invalid_clock(sharedOutput.get()),
            (unsigned long long)lc_shared_output_missing_frames(sharedOutput.get()),(unsigned long long)steadyMissing);
        if(shared) std::printf("received_frames=%llu initial_missing_frames=%llu missing_after_first_read=%llu\n",
            (unsigned long long)lc_shared_output_received_frames(sharedOutput.get()),
            (unsigned long long)lc_shared_output_initial_missing_frames(sharedOutput.get()),
            (unsigned long long)(lc_shared_output_missing_frames(sharedOutput.get())-lc_shared_output_initial_missing_frames(sharedOutput.get())));
        if(aggregate) std::printf("bridge_callbacks=%llu bridge_frames=%u..%u missing_bridge_frames=%llu\n",(unsigned long long)lc_bridge_callbacks(bridge.get()),lc_bridge_min_frames(bridge.get()),lc_bridge_max_frames(bridge.get()),(unsigned long long)lc_bridge_missing_frames(bridge.get()));
        std::printf("callback_frames send=%u..%u receive=%u..%u\n",send.minFrames,send.maxFrames,receive.minFrames,receive.maxFrames);
        if(scheduled.empty()||send.badLayout||receive.badLayout) throw std::runtime_error("No valid latency result");
        report("sample_timestamp_delta",scheduled); report("submit_to_receive_callback",age);
        std::puts("Digital virtual path only. Timestamp delta and callback age are distinct; neither measures analog hardware latency.");
        return lost||receive.invalid||receive.duplicates||(shared&&(steadyMissing||lc_shared_output_invalid_clock(sharedOutput.get())))||scheduled.size()<20||send.minFrames!=frames||send.maxFrames!=frames||receive.minFrames!=frames||receive.maxFrames!=frames?1:0;
    } catch(const std::exception &e) { std::fprintf(stderr,"ERROR: %s\n",e.what()); return 1; }
}
#ifndef PATCHLANE_EMBED_LATENCY_CHECK
int main(int argc,char **argv) { return runLoopbackLatency(argc,argv); }
#endif
