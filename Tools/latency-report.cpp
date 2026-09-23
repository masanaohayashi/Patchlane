// Read-only inventory. Reported properties are not measured end-to-end latency.
// clang++ -std=c++17 Tools/latency-report.cpp -framework CoreAudio -framework CoreFoundation -o .build/latency-report
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <vector>

template<class T> bool read(AudioObjectID id, AudioObjectPropertySelector key,
                           AudioObjectPropertyScope scope, T &value) {
    AudioObjectPropertyAddress a{key,scope,kAudioObjectPropertyElementMain};
    UInt32 size=sizeof(value);
    return AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&value)==noErr && size==sizeof(value);
}
std::vector<AudioObjectID> objects(AudioObjectID id, AudioObjectPropertySelector key,
                                  AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a{key,scope,kAudioObjectPropertyElementMain};
    UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(id,&a,0,nullptr,&size)!=noErr) return {};
    std::vector<AudioObjectID> result(size/sizeof(AudioObjectID));
    if(size && AudioObjectGetPropertyData(id,&a,0,nullptr,&size,result.data())!=noErr) return {};
    result.resize(size/sizeof(AudioObjectID));
    return result;
}
void number(AudioObjectID id, AudioObjectPropertySelector key,
            AudioObjectPropertyScope scope, const char *label) {
    UInt32 value=0;
    if(read(id,key,scope,value)) std::printf(" %s=%u",label,value);
    else std::printf(" %s=unavailable",label);
}
int main() {
    constexpr auto global=kAudioObjectPropertyScopeGlobal;
    std::puts("Read-only HAL properties; frame counts, NOT end-to-end measurements.");
    auto devices=objects(kAudioObjectSystemObject,kAudioHardwarePropertyDevices,global);
    if(devices.empty()) return 1;
    for(auto device:devices) {
        char name[512]="unknown"; CFStringRef text=nullptr;
        if(read(device,kAudioObjectPropertyName,global,text) && text) {
            CFStringGetCString(text,name,sizeof(name),kCFStringEncodingUTF8); CFRelease(text);
        }
        std::printf("%s (id=%u)",name,device);
        double rate=0;
        if(read(device,kAudioDevicePropertyNominalSampleRate,global,rate)) std::printf(" rate=%.0f",rate);
        number(device,kAudioDevicePropertyBufferFrameSize,global,"buffer");
        number(device,kAudioDevicePropertyClockDomain,global,"clockDomain");
        number(device,kAudioDevicePropertyDeviceIsRunningSomewhere,global,"running");
        AudioValueRange range{};
        if(read(device,kAudioDevicePropertyBufferFrameSizeRange,global,range))
            std::printf(" bufferRange=%.0f..%.0f",range.mMinimum,range.mMaximum);
        std::puts("");
        for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}) {
            auto streams=objects(device,kAudioDevicePropertyStreams,scope);
            if(streams.empty()) continue;
            std::printf("  %s",scope==kAudioObjectPropertyScopeInput?"input":"output");
            number(device,kAudioDevicePropertyLatency,scope,"deviceLatency");
            number(device,kAudioDevicePropertySafetyOffset,scope,"safetyOffset");
            for(auto stream:streams) number(stream,kAudioStreamPropertyLatency,global,"streamLatency");
            std::puts("");
        }
    }
}
