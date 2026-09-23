#include "AudioCore.h"
#include <CoreAudio/CoreAudio.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
template<typename T> bool read(AudioDeviceID id, AudioObjectPropertySelector selector,T &value) {
    AudioObjectPropertyAddress a{selector,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 n=sizeof(T);
    return AudioObjectGetPropertyData(id,&a,0,nullptr,&n,&value)==0;
}
int main() {
    std::vector<LCDevice> devices(64);int n=lc_devices(devices.data(),64);
    for(int i=0;i<n && i<64;++i) {
        auto &d=devices[i];if(!std::strstr(d.name,"BlackHole") || !d.outputs)continue;
        UInt32 busy=1;if(!read(d.id,kAudioDevicePropertyDeviceIsRunningSomewhere,busy)||busy)continue;
        UInt32 originalFrames=0;double originalRate=0;
        if(!read(d.id,kAudioDevicePropertyBufferFrameSize,originalFrames)||!read(d.id,kAudioDevicePropertyNominalSampleRate,originalRate))return 1;
        for(int frames:{32,128}) {
            auto *e=lc_create();lc_config_timing(e,frames,44100);lc_config_output(e,0,d.id);
            if(lc_start(e)!=0){std::printf("FAIL %s: %s\n",d.name,lc_error(e));lc_destroy(e);return 1;}
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            UInt32 actual=0;double rate=0;read(d.id,kAudioDevicePropertyBufferFrameSize,actual);read(d.id,kAudioDevicePropertyNominalSampleRate,rate);
            auto callback=lc_output_callback_frames(e,0);
            lc_stop(e);lc_destroy(e);
            UInt32 restored=0;double restoredRate=0;read(d.id,kAudioDevicePropertyBufferFrameSize,restored);read(d.id,kAudioDevicePropertyNominalSampleRate,restoredRate);
            bool ok=actual==unsigned(frames)&&callback==unsigned(frames)&&rate==44100&&restored==originalFrames&&restoredRate==originalRate;
            std::printf("%s %s requested=%d actual=%u callback=%u rate=%.0f restored=%u/%.0f\n",ok?"PASS":"FAIL",d.name,frames,actual,callback,rate,restored,restoredRate);
            if(!ok)return 1;
        }
        return 0;
    }
    std::puts("SKIP: no idle BlackHole device; active audio was not changed.");return 77;
}
