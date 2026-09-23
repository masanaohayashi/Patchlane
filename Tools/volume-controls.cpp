// Read-only check of the live HAL controls used by the macOS sound menu.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <vector>
int main() {
    AudioObjectPropertyAddress list{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,0}; UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&list,0,nullptr,&size)) return 1;
    std::vector<AudioDeviceID> ids(size/4); if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&list,0,nullptr,&size,ids.data())) return 1;
    bool found=false,ok=false;
    for(auto id:ids) {
        AudioObjectPropertyAddress uid{kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,0}; CFStringRef value=nullptr; UInt32 n=sizeof(value);
        if(AudioObjectGetPropertyData(id,&uid,0,nullptr,&n,&value)||!value) continue;
        bool match=CFEqual(value,CFSTR("audio.patchlane.virtual8")); CFRelease(value); if(!match) continue;
        found=true; ok=true;
        for(auto key:{kAudioDevicePropertyVolumeScalar,kAudioDevicePropertyMute}) {
            AudioObjectPropertyAddress a{key,kAudioObjectPropertyScopeOutput,kAudioObjectPropertyElementMain}; Boolean settable=false;
            bool exists=AudioObjectHasProperty(id,&a);
            OSStatus e=exists?AudioObjectIsPropertySettable(id,&a,&settable):-1;
            std::printf("%s %s exists=%d writable=%d status=%d",exists&&!e&&settable?"PASS":"FAIL",key==kAudioDevicePropertyVolumeScalar?"output master volume":"output master mute",exists,settable,int(e));
            ok &= exists&&!e&&settable;
            if(exists) {
                UInt32 bytes=4;
                if(key==kAudioDevicePropertyVolumeScalar) { float v=0; if(!AudioObjectGetPropertyData(id,&a,0,nullptr,&bytes,&v)) std::printf(" scalar=%g",v); }
                else { UInt32 v=0; if(!AudioObjectGetPropertyData(id,&a,0,nullptr,&bytes,&v)) std::printf(" muted=%u",v); }
            }
            std::puts("");
        }
    }
    if(!found) std::puts("FAIL dedicated device not loaded");
    return found&&ok?0:1;
}
