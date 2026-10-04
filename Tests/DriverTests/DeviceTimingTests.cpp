#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <cstring>
#include <cassert>
static double deviceRate=48000;
static UInt32 deviceFrames=512;
static bool supports44100=false,rejectRate=false;
static int rateWrites=0;
static OSStatus fakeSize(AudioObjectID,const AudioObjectPropertyAddress *a,UInt32,const void*,UInt32 *size) {
    if(a->mSelector!=kAudioDevicePropertyAvailableNominalSampleRates)return kAudioHardwareUnknownPropertyError;
    *size=sizeof(AudioValueRange);return noErr;
}
static OSStatus fakeGet(AudioObjectID,const AudioObjectPropertyAddress *a,UInt32,const void*,UInt32 *size,void *data) {
    if(a->mSelector==kAudioDevicePropertyNominalSampleRate){std::memcpy(data,&deviceRate,sizeof(deviceRate));return noErr;}
    if(a->mSelector==kAudioDevicePropertyBufferFrameSize){std::memcpy(data,&deviceFrames,sizeof(deviceFrames));return noErr;}
    if(a->mSelector==kAudioDevicePropertyAvailableNominalSampleRates){AudioValueRange range{supports44100?44100.:48000.,48000.};*size=sizeof(range);std::memcpy(data,&range,sizeof(range));return noErr;}
    return kAudioHardwareUnknownPropertyError;
}
static OSStatus fakeSet(AudioObjectID,const AudioObjectPropertyAddress *a,UInt32,const void*,UInt32,const void *data) {
    if(a->mSelector==kAudioDevicePropertyNominalSampleRate){++rateWrites;double value=*static_cast<const double*>(data);if(rejectRate||(!supports44100&&value!=48000))return kAudioHardwareUnspecifiedError;deviceRate=value;return noErr;}
    if(a->mSelector==kAudioDevicePropertyBufferFrameSize){deviceFrames=*static_cast<const UInt32*>(data);return noErr;}
    return kAudioHardwareUnknownPropertyError;
}
#define AudioObjectGetPropertyDataSize fakeSize
#define AudioObjectGetPropertyData fakeGet
#define AudioObjectSetPropertyData fakeSet
#include "../../Sources/AudioCore/AudioCore.cpp"
int main(){
    LCEngine engine;engine.blockFrames=64;engine.systemRate=44100;
    assert(engine.prepareDevice(1)); // Fixed-48kHz device must remain usable.
    assert(deviceRate==48000&&deviceFrames==64&&rateWrites==0);
    engine.restoreDevices();assert(deviceRate==48000&&deviceFrames==512);
    supports44100=true;
    assert(engine.prepareDevice(1));assert(deviceRate==44100&&deviceFrames==64);
    engine.restoreDevices();assert(deviceRate==48000&&deviceFrames==512);
    rejectRate=true;
    assert(!engine.prepareDevice(1)); // Real errors must not be swallowed.
    engine.restoreDevices();
    std::puts("PASS fixed-rate fallback, supported-rate configuration, restoration, genuine failure");
}
