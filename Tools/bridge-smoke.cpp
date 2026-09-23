#include "AudioCore.h"
#include <CoreAudio/CoreAudio.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
template<class T> bool read(AudioDeviceID id,UInt32 key,T &value) {
    AudioObjectPropertyAddress a{key,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}; UInt32 size=sizeof(value);
    return AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&value)==0&&size==sizeof(value);
}
int main() {
    std::vector<LCDevice> devices(lc_devices(nullptr,0)+16); int count=lc_devices(devices.data(),int(devices.size()));
    AudioDeviceID source=0,destination=0;
    for(int i=0;i<count&&i<int(devices.size());++i) {
        UInt32 active=1; if(!read(devices[i].id,kAudioDevicePropertyDeviceIsRunningSomewhere,active)||active) continue;
        if(std::strcmp(devices[i].uid,"BlackHole16ch_UID")==0) source=devices[i].id;
        if(std::strcmp(devices[i].name,"ZOOM AMS-24 Driver")==0) destination=devices[i].id;
    }
    if(!source||!destination) { std::puts("SKIP: requires idle BlackHole 16ch and ZOOM AMS-24"); return 77; }
    UInt32 beforeFrames[2]; double beforeRates[2]; AudioDeviceID ids[]{source,destination};
    for(unsigned i=0;i<2;++i) if(!read(ids[i],kAudioDevicePropertyBufferFrameSize,beforeFrames[i])||!read(ids[i],kAudioDevicePropertyNominalSampleRate,beforeRates[i])) return 1;
    auto bridge=std::unique_ptr<LCBridge,decltype(&lc_bridge_destroy)>(lc_bridge_create(),lc_bridge_destroy);
    if(lc_bridge_start(bridge.get(),source,destination,0,0,1,32,44100)) { std::printf("FAIL %s\n",lc_bridge_error(bridge.get())); return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto callbacks=lc_bridge_callbacks(bridge.get()); auto minimum=lc_bridge_min_frames(bridge.get()),maximum=lc_bridge_max_frames(bridge.get());
    auto missing=lc_bridge_missing_frames(bridge.get()); auto peak=lc_bridge_peak(bridge.get());
    lc_bridge_stop(bridge.get());
    bool restored=true;
    for(unsigned i=0;i<2;++i) {
        UInt32 frames=0; double rate=0;
        restored &= read(ids[i],kAudioDevicePropertyBufferFrameSize,frames)&&read(ids[i],kAudioDevicePropertyNominalSampleRate,rate)&&frames==beforeFrames[i]&&rate==beforeRates[i];
    }
    const bool ok=callbacks>10&&minimum==32&&maximum==32&&missing==0&&peak==0&&restored;
    std::printf("%s idle BlackHole -> ZOOM, callbacks=%llu frames=%u..%u unavailable=%llu peak=%g restored=%d\n",ok?"PASS":"FAIL",(unsigned long long)callbacks,minimum,maximum,(unsigned long long)missing,peak,restored);
    return ok?0:1;
}
