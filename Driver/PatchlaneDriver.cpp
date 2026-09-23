// Original Patchlane HAL driver. API contracts checked against Apple's
// AudioServerPlugIn.h and Creating an Audio Server Driver Plug-in sample.
#include "DriverDSP.hpp"
#include "../Shared/BrokerClient.hpp"
#include "BrokerRequirement.hpp"
#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <set>
#include <limits>

namespace {
#if LCD_CHANNELS == 2
#define LCD_DEVICE_NAME "Patchlane 2ch"
#define LCD_DEVICE_UID "audio.patchlane.virtual2"
#define LCD_MODEL_UID "audio.patchlane.virtual2.v1"
#define LCD_BUNDLE_ID "audio.patchlane.driver.stereo"
#else
#define LCD_DEVICE_NAME "Patchlane 8ch"
#define LCD_DEVICE_UID "audio.patchlane.virtual8"
#define LCD_MODEL_UID "audio.patchlane.virtual8.v1"
#define LCD_BUNDLE_ID "audio.patchlane.driver"
#endif
constexpr AudioObjectID Plugin=1,Device=2,Input=3,Output=4,Volume=5,Mute=6;
constexpr auto Global=kAudioObjectPropertyScopeGlobal;
constexpr auto In=kAudioObjectPropertyScopeInput,Out=kAudioObjectPropertyScopeOutput;
constexpr AudioObjectPropertySelector Configuration='lcfg',Statistics='lsta',SharedStatus='lshm';
constexpr uint32_t ClockPeriod=16384;
AudioServerPlugInDriverRef driverRef;
AudioServerPlugInHostRef host=nullptr;
lcd::DSP dsp;
lcshared::MappedAudioRing sharedAudio;
dispatch_source_t brokerPublisher=nullptr;
std::atomic<uint64_t> sharedBytes{0},publishAttempts{0};
std::atomic<kern_return_t> sharedAllocation{KERN_FAILURE},publishResult{KERN_FAILURE};
std::atomic<UInt32> references{1},running{0};
std::atomic<bool> inputActive{true},outputActive{true},outputMuted{false};
std::atomic<float> outputVolume{1.f};
constexpr float MinDB=-96.f;
float scalarDB(float v) { return v<=0 ? MinDB:std::max(MinDB,20.f*std::log10(v)); }
float dbScalar(float v) { return v<=MinDB ? 0.f:std::pow(10.f,std::min(0.f,v)/20.f); }
std::atomic<double> sampleRate{44100},ticksPerFrame{0};
std::atomic<uint64_t> anchor{0},seed{1},clockSequence{0};
std::mutex state;
std::set<UInt32> started;
uint64_t pendingRate=0;
double ticksPerSecond=0;
const double Rates[]{44100,48000,88200,96000};
const float Silence[lcd::MaxFrames*lcd::Channels]{};
bool rateOK(double r) { for(auto v:Rates) if(r==v) return true; return false; }
bool objectOK(AudioObjectID o) { return o>=Plugin&&o<=Mute; }
bool deviceOK(AudioServerPlugInDriverRef r,AudioObjectID o) { return r==driverRef&&o==Device; }
void notify(AudioObjectID o,AudioObjectPropertySelector s) {
    if(host) { AudioObjectPropertyAddress a{s,Global,kAudioObjectPropertyElementMain}; host->PropertiesChanged(host,o,1,&a); }
}
AudioStreamBasicDescription format(double rate) {
    AudioStreamBasicDescription f{}; f.mSampleRate=rate; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagsNativeFloatPacked; f.mBytesPerPacket=f.mBytesPerFrame=lcd::Channels*4;
    f.mFramesPerPacket=1; f.mChannelsPerFrame=lcd::Channels; f.mBitsPerChannel=32; return f;
}
bool formatOK(const AudioStreamBasicDescription &f) {
    auto expected=format(f.mSampleRate);
    return rateOK(f.mSampleRate)&&f.mFormatID==expected.mFormatID&&f.mFormatFlags==expected.mFormatFlags&&
        f.mBytesPerPacket==expected.mBytesPerPacket&&f.mBytesPerFrame==expected.mBytesPerFrame&&
        f.mFramesPerPacket==1&&f.mChannelsPerFrame==lcd::Channels&&f.mBitsPerChannel==32;
}
// Control-thread property serializer. Size queries never allocate CF objects.
struct Reply {
    UInt32 capacity; UInt32 *size; void *data;
    OSStatus bytes(const void *p,UInt32 n,bool array=false,UInt32 element=4) {
        if(!size) return kAudioHardwareIllegalOperationError;
        if(!data) { *size=n; return noErr; }
        if(array) n=std::min(n,capacity/element*element);
        else if(capacity<n) return kAudioHardwareBadPropertySizeError;
        if(n) std::memcpy(data,p,n); *size=n; return noErr;
    }
    template<class T> OSStatus value(T v) { return bytes(&v,sizeof(v)); }
    OSStatus string(CFStringRef s) {
        if(!data) return value(s);
        if(capacity<sizeof(s)) return kAudioHardwareBadPropertySizeError;
        CFRetain(s); return value(s);
    }
    OSStatus blob(const void *p,size_t n) {
        if(!data) return value(CFDataRef(nullptr));
        if(capacity<sizeof(CFDataRef)) return kAudioHardwareBadPropertySizeError;
        auto result=CFDataCreate(nullptr,static_cast<const UInt8*>(p),n);
        if(!result) return kAudioHardwareUnspecifiedError;
        return value(result);
    }
};
OSStatus property(AudioObjectID o,const AudioObjectPropertyAddress &a,UInt32 qs,const void *q,Reply r) {
    if(!objectOK(o)) return kAudioHardwareBadObjectError;
    if(a.mElement!=kAudioObjectPropertyElementMain) return kAudioHardwareUnknownPropertyError;
    if(a.mScope!=Global&&a.mScope!=In&&a.mScope!=Out) return kAudioHardwareUnknownPropertyError;
    const auto s=a.mSelector;
    if(o==Volume||o==Mute) {
        if(a.mScope!=Global) return kAudioHardwareUnknownPropertyError;
        switch(s) {
            case kAudioObjectPropertyBaseClass: return r.value(UInt32(o==Volume?kAudioLevelControlClassID:kAudioBooleanControlClassID));
            case kAudioObjectPropertyClass: return r.value(UInt32(o==Volume?kAudioVolumeControlClassID:kAudioMuteControlClassID));
            case kAudioObjectPropertyOwner: return r.value(UInt32(Device));
            case kAudioObjectPropertyOwnedObjects: return r.bytes(nullptr,0,true);
            case kAudioObjectPropertyName: return r.string(o==Volume?CFSTR("Output Volume"):CFSTR("Output Mute"));
            case kAudioControlPropertyScope: return r.value(UInt32(Out));
            case kAudioControlPropertyElement: return r.value(UInt32(kAudioObjectPropertyElementMain));
        }
        if(o==Mute) return s==kAudioBooleanControlPropertyValue?r.value(UInt32(outputMuted.load())):kAudioHardwareUnknownPropertyError;
        switch(s) {
            case kAudioLevelControlPropertyScalarValue: return r.value(outputVolume.load());
            case kAudioLevelControlPropertyDecibelValue: return r.value(scalarDB(outputVolume.load()));
            case kAudioLevelControlPropertyDecibelRange: return r.value(AudioValueRange{MinDB,0});
            case kAudioLevelControlPropertyConvertScalarToDecibels:
            case kAudioLevelControlPropertyConvertDecibelsToScalar: {
                if(!r.data) return r.value(float(0));
                if(r.capacity<sizeof(float)) return kAudioHardwareBadPropertySizeError;
                float v; std::memcpy(&v,r.data,sizeof(v));
                if(!std::isfinite(v)) return kAudioHardwareIllegalOperationError;
                return r.value(s==kAudioLevelControlPropertyConvertScalarToDecibels?scalarDB(std::clamp(v,0.f,1.f)):dbScalar(v));
            }
        }
        return kAudioHardwareUnknownPropertyError;
    }
    if(s==kAudioObjectPropertyBaseClass) return r.value(UInt32(kAudioObjectClassID));
    if(s==kAudioObjectPropertyClass) return r.value(UInt32(o==Plugin?kAudioPlugInClassID:o==Device?kAudioDeviceClassID:kAudioStreamClassID));
    if(s==kAudioObjectPropertyOwner) return r.value(UInt32(o==Plugin?kAudioObjectUnknown:o==Device?Plugin:Device));
    if(s==kAudioObjectPropertyManufacturer) return r.string(CFSTR("Patchlane"));
    if(s==kAudioObjectPropertyName) return r.string(o==Plugin?CFSTR("Patchlane Driver"):o==Device?CFSTR(LCD_DEVICE_NAME):lcd::Channels==2?CFSTR("Stereo L / R"):o==Input?CFSTR("Main / Aux 1 / Aux 2 / Aux 3"):CFSTR("Input 1 / Input 2 / Input 3 / Input 4"));
    if(s==kAudioObjectPropertyOwnedObjects) {
        AudioObjectID ids[4]{}; UInt32 count=0;
        if(o==Plugin) ids[count++]=Device;
        if(o==Device) { if(a.mScope!=Out) ids[count++]=Input; if(a.mScope!=In) { ids[count++]=Output; ids[count++]=Volume; ids[count++]=Mute; } }
        return r.bytes(ids,count*4,true);
    }
    if(o==Plugin) {
        if(s==kAudioPlugInPropertyBundleID) return r.string(CFSTR(LCD_BUNDLE_ID));
        if(s==kAudioPlugInPropertyDeviceList) { AudioObjectID id=Device; return r.bytes(&id,4,true); }
        if(s==kAudioPlugInPropertyResourceBundle) return r.string(CFSTR(""));
        if(s==kAudioPlugInPropertyTranslateUIDToDevice) {
            if(qs!=sizeof(CFStringRef)||!q) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid=*static_cast<const CFStringRef*>(q);
            if(!uid||CFGetTypeID(uid)!=CFStringGetTypeID()) return kAudioHardwareIllegalOperationError;
            return r.value(UInt32(CFEqual(uid,CFSTR(LCD_DEVICE_UID))?Device:kAudioObjectUnknown));
        }
    } else if(o==Device) {
        switch(s) {
            case kAudioDevicePropertyDeviceUID: return r.string(CFSTR(LCD_DEVICE_UID));
            case kAudioDevicePropertyModelUID: return r.string(CFSTR(LCD_MODEL_UID));
            case kAudioDevicePropertyTransportType: return r.value(UInt32(kAudioDeviceTransportTypeVirtual));
            case kAudioDevicePropertyClockDomain: return r.value(UInt32(0));
            case kAudioDevicePropertyDeviceIsAlive: return r.value(UInt32(1));
            case kAudioDevicePropertyDeviceIsRunning: return r.value(running.load());
            case kAudioDevicePropertyDeviceCanBeDefaultDevice: return a.mScope==Global?kAudioHardwareUnknownPropertyError:r.value(UInt32(1));
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return a.mScope==Global?kAudioHardwareUnknownPropertyError:r.value(UInt32(0));
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset: return a.mScope==Global?kAudioHardwareUnknownPropertyError:r.value(UInt32(0));
            case kAudioDevicePropertyIsHidden: return r.value(UInt32(0));
            case kAudioDevicePropertyZeroTimeStampPeriod: return r.value(ClockPeriod);
            case kAudioDevicePropertyRelatedDevices: { AudioObjectID id=Device; return r.bytes(&id,4,true); }
            case kAudioObjectPropertyControlList: { AudioObjectID ids[]{Volume,Mute}; return r.bytes(ids,a.mScope==In?0:sizeof(ids),true); }
            case kAudioDevicePropertyStreams: {
                AudioObjectID ids[2]; UInt32 count=0;
                if(a.mScope!=Out) ids[count++]=Input; if(a.mScope!=In) ids[count++]=Output;
                return r.bytes(ids,count*4,true);
            }
            case kAudioDevicePropertyNominalSampleRate: return r.value(sampleRate.load());
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                AudioValueRange ranges[4]; for(unsigned i=0;i<4;++i) ranges[i]={Rates[i],Rates[i]};
                return r.bytes(ranges,sizeof(ranges),true,sizeof(ranges[0]));
            }
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                if(a.mScope==Global) return kAudioHardwareUnknownPropertyError;
                UInt32 stereo[]{1,2}; return r.bytes(stereo,sizeof(stereo));
            }
            case kAudioDevicePropertyPreferredChannelLayout: {
                if(a.mScope==Global) return kAudioHardwareUnknownPropertyError;
                struct Layout { AudioChannelLayoutTag tag; AudioChannelBitmap bitmap; UInt32 count; AudioChannelDescription channels[lcd::Channels]; } layout{};
                layout.tag=kAudioChannelLayoutTag_UseChannelDescriptions; layout.count=lcd::Channels;
                for(unsigned c=0;c<lcd::Channels;++c) layout.channels[c].mChannelLabel=lcd::Channels==2?(c==0?kAudioChannelLabel_Left:kAudioChannelLabel_Right):kAudioChannelLabel_Discrete_0+c;
                return r.bytes(&layout,sizeof(layout));
            }
            case kAudioObjectPropertyCustomPropertyInfoList: {
                AudioServerPlugInCustomPropertyInfo info[]{
                    {Configuration,kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,kAudioServerPlugInCustomPropertyDataTypeNone},
                    {SharedStatus,kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,kAudioServerPlugInCustomPropertyDataTypeNone},
                    {Statistics,kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,kAudioServerPlugInCustomPropertyDataTypeNone}};
                return r.bytes(info,sizeof(info),true,sizeof(info[0]));
            }
            case Configuration: { auto c=dsp.controls.get(); return r.blob(&c,sizeof(c)); }
            case SharedStatus: {
                // Diagnostic counters may advance between fields. These are not
                // a synchronization mechanism for the audio reader.
                uint64_t values[]{1,lcshared::Version,sharedBytes.load(),uint64_t(int64_t(sharedAllocation.load())),
                                  publishAttempts.load(),uint64_t(int64_t(publishResult.load()))};
                return r.blob(values,sizeof(values));
            }
            case Statistics: {
                uint64_t values[]{1,dsp.written.load(),dsp.readFrames.load(),dsp.missing.load(),seed.load()};
                return r.blob(values,sizeof(values));
            }
        }
    } else {
        switch(s) {
            case kAudioStreamPropertyIsActive: return r.value(UInt32(o==Input?inputActive.load():outputActive.load()));
            case kAudioStreamPropertyDirection: return r.value(UInt32(o==Input));
            case kAudioStreamPropertyTerminalType: return r.value(UInt32(kAudioStreamTerminalTypeLine));
            case kAudioStreamPropertyStartingChannel: return r.value(UInt32(1));
            case kAudioStreamPropertyLatency: return r.value(UInt32(0));
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: return r.value(format(sampleRate.load()));
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats: {
                AudioStreamRangedDescription formats[4];
                for(unsigned i=0;i<4;++i) formats[i]={format(Rates[i]),{Rates[i],Rates[i]}};
                return r.bytes(formats,sizeof(formats),true,sizeof(formats[0]));
            }
        }
    }
    return kAudioHardwareUnknownPropertyError;
}
Boolean has(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t,const AudioObjectPropertyAddress *a) {
    if(d!=driverRef||!a) return false;
    // Translation needs a qualifier only for Get, not for existence/size.
    if(o==Plugin&&a->mSelector==kAudioPlugInPropertyTranslateUIDToDevice) return true;
    UInt32 size=0; return property(o,*a,0,nullptr,{0,&size,nullptr})==noErr;
}
OSStatus settable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress *a,Boolean *out) {
    if(!out||!a) return kAudioHardwareIllegalOperationError;
    if(!has(d,o,p,a)) return kAudioHardwareUnknownPropertyError;
    auto s=a->mSelector;
    *out=(o==Volume&&(s==kAudioLevelControlPropertyScalarValue||s==kAudioLevelControlPropertyDecibelValue))||
        (o==Mute&&s==kAudioBooleanControlPropertyValue)||(o==Device&&(s==Configuration||s==kAudioDevicePropertyNominalSampleRate))||
        ((o==Input||o==Output)&&(s==kAudioStreamPropertyIsActive||s==kAudioStreamPropertyVirtualFormat||s==kAudioStreamPropertyPhysicalFormat));
    return noErr;
}
OSStatus dataSize(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *size) {
    if(d!=driverRef||!objectOK(o)) return kAudioHardwareBadObjectError;
    if(!a||!size) return kAudioHardwareIllegalOperationError;
    if(o==Plugin&&a->mSelector==kAudioPlugInPropertyTranslateUIDToDevice) { *size=4; return noErr; }
    return property(o,*a,qs,q,{0,size,nullptr});
}
OSStatus get(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 size,UInt32 *used,void *out) {
    if(d!=driverRef) return kAudioHardwareBadObjectError;
    if(!a||!used||(!out&&size)) return kAudioHardwareIllegalOperationError;
    // Empty array reads may legitimately have zero capacity and a null buffer.
    if(!out) { UInt32 needed=0; auto e=property(o,*a,qs,q,{0,&needed,nullptr}); *used=0; return e?e:needed?kAudioHardwareBadPropertySizeError:noErr; }
    return property(o,*a,qs,q,{size,used,out});
}
OSStatus requestRate(double rate) {
    if(!rateOK(rate)) return kAudioHardwareIllegalOperationError;
    std::lock_guard<std::mutex> lock(state);
    if(pendingRate) return pendingRate==uint64_t(rate)?noErr:kAudioHardwareIllegalOperationError;
    if(rate==sampleRate.load()) return noErr;
    if(!host) return kAudioHardwareNotReadyError;
    pendingRate=uint64_t(rate);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        auto err=host->RequestDeviceConfigurationChange(host,Device,uint64_t(rate),nullptr);
        if(err) { std::lock_guard<std::mutex> guard(state); if(pendingRate==uint64_t(rate)) pendingRate=0; }
    });
    return noErr;
}
OSStatus set(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress *a,UInt32,const void*,UInt32 size,const void *value) {
    if(d!=driverRef||!objectOK(o)) return kAudioHardwareBadObjectError;
    if(!a||!value) return kAudioHardwareIllegalOperationError;
    Boolean can=false; auto err=settable(d,o,p,a,&can); if(err) return err;
    if(!can) return kAudioHardwareIllegalOperationError;
    const auto s=a->mSelector;
    if(o==Volume) {
        if(size!=sizeof(float)) return kAudioHardwareBadPropertySizeError;
        float v; std::memcpy(&v,value,sizeof(v));
        if(!std::isfinite(v)) return kAudioHardwareIllegalOperationError;
        v=s==kAudioLevelControlPropertyDecibelValue?dbScalar(v):std::clamp(v,0.f,1.f);
        if(outputVolume.exchange(v)!=v) {
            AudioObjectPropertyAddress addresses[]{
                {kAudioLevelControlPropertyScalarValue,Global,kAudioObjectPropertyElementMain},
                {kAudioLevelControlPropertyDecibelValue,Global,kAudioObjectPropertyElementMain}};
            if(host) host->PropertiesChanged(host,Volume,2,addresses);
        }
        return noErr;
    }
    if(o==Mute) {
        if(size!=sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        bool v=*static_cast<const UInt32*>(value)!=0;
        if(outputMuted.exchange(v)!=v) notify(Mute,kAudioBooleanControlPropertyValue);
        return noErr;
    }
    if(s==Configuration) {
        if(size!=sizeof(CFDataRef)) return kAudioHardwareBadPropertySizeError;
        auto data=*static_cast<const CFDataRef*>(value);
        if(!data||CFGetTypeID(data)!=CFDataGetTypeID()||CFDataGetLength(data)!=sizeof(lcd::Config)) return kAudioHardwareIllegalOperationError;
        lcd::Config c; std::memcpy(&c,CFDataGetBytePtr(data),sizeof(c));
        if(!dsp.controls.publish(c)) return kAudioHardwareIllegalOperationError;
        notify(Device,Configuration); return noErr;
    }
    if(s==kAudioDevicePropertyNominalSampleRate) {
        if(size!=sizeof(double)) return kAudioHardwareBadPropertySizeError;
        return requestRate(*static_cast<const double*>(value));
    }
    if(s==kAudioStreamPropertyIsActive) {
        if(size!=sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        (o==Input?inputActive:outputActive).store(*static_cast<const UInt32*>(value)!=0);
        notify(o,s); return noErr;
    }
    if(size!=sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
    auto f=*static_cast<const AudioStreamBasicDescription*>(value);
    return formatOK(f)?requestRate(f.mSampleRate):kAudioDeviceUnsupportedFormatError;
}
HRESULT query(void *d,REFIID id,LPVOID *out) {
    if(d!=driverRef||!out) return E_NOINTERFACE;
    *out=nullptr;
    auto uuid=CFUUIDCreateFromUUIDBytes(nullptr,id);
    const bool match=uuid&&(CFEqual(uuid,IUnknownUUID)||CFEqual(uuid,kAudioServerPlugInDriverInterfaceUUID));
    if(uuid) CFRelease(uuid);
    if(!match) return E_NOINTERFACE;
    references.fetch_add(1); *out=driverRef; return S_OK;
}
ULONG addRef(void *d) { return d==driverRef?references.fetch_add(1)+1:0; }
ULONG release(void *d) {
    if(d!=driverRef) return 0;
    auto n=references.load(); while(n&&!references.compare_exchange_weak(n,n-1)) {} return n?n-1:0;
}
OSStatus initialize(AudioServerPlugInDriverRef d,AudioServerPlugInHostRef h) {
    if(d!=driverRef||!h) return kAudioHardwareIllegalOperationError;
    if(!brokerPublisher) sharedAllocation=sharedAudio.create();
    if(!brokerPublisher && sharedAllocation.load()==KERN_SUCCESS) {
        sharedBytes=sharedAudio.mappedSize();
        dsp.attachRing(sharedAudio.producer());
        // Setup/reconnection is serialized away from HAL's real-time threads.
        auto queue=dispatch_queue_create("audio.patchlane.broker",DISPATCH_QUEUE_SERIAL);
        brokerPublisher=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue);
        dispatch_source_set_timer(brokerPublisher,dispatch_time(DISPATCH_TIME_NOW,0),10*NSEC_PER_SEC,NSEC_PER_SEC);
        dispatch_source_set_event_handler(brokerPublisher,^{
            publishResult=lcshared::brokerExchange(sharedAudio,true,lcshared::BrokerExecutable,0,CFSTR(LCD_BROKER_REQUIREMENT),3000,lcd::Channels==2);
            publishAttempts.fetch_add(1);
        });
        dispatch_resume(brokerPublisher);dispatch_release(queue);
    }
    host=h; mach_timebase_info_data_t timebase{}; mach_timebase_info(&timebase);
    ticksPerSecond=1e9*double(timebase.denom)/timebase.numer;
    ticksPerFrame=ticksPerSecond/sampleRate.load(); anchor=mach_absolute_time(); return noErr;
}
OSStatus createDevice(AudioServerPlugInDriverRef,CFDictionaryRef,const AudioServerPlugInClientInfo*,AudioObjectID*) { return kAudioHardwareUnsupportedOperationError; }
OSStatus destroyDevice(AudioServerPlugInDriverRef,AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }
OSStatus client(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c) { return deviceOK(d,o)&&c?noErr:kAudioHardwareBadObjectError; }
OSStatus perform(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 action,void*) {
    if(!deviceOK(d,o)) return kAudioHardwareBadObjectError;
    {
        std::lock_guard<std::mutex> lock(state);
        if(!rateOK(double(action))||pendingRate!=action) return kAudioHardwareIllegalOperationError;
        // A failed execution consumes this request too. Otherwise the same-rate
        // retry is mistaken for a still-pending request and never reaches HAL.
        pendingRate=0;
        if(running.load()) return kAudioHardwareIllegalOperationError;
        clockSequence.fetch_add(1);
        sampleRate=double(action); ticksPerFrame=ticksPerSecond/double(action);
        anchor=mach_absolute_time(); seed.fetch_add(1); dsp.reset(); pendingRate=0;
        clockSequence.fetch_add(1);
    }
    notify(Device,kAudioDevicePropertyNominalSampleRate);
    for(auto stream:{Input,Output}) { notify(stream,kAudioStreamPropertyVirtualFormat); notify(stream,kAudioStreamPropertyPhysicalFormat); }
    return noErr;
}
OSStatus abortChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 action,void*) {
    if(!deviceOK(d,o)) return kAudioHardwareBadObjectError;
    std::lock_guard<std::mutex> lock(state); if(pendingRate==action) pendingRate=0; return noErr;
}
OSStatus start(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id) {
    if(!deviceOK(d,o)) return kAudioHardwareBadObjectError;
    bool changed=false;
    {
        std::lock_guard<std::mutex> lock(state);
        if(started.count(id)) return kAudioHardwareIllegalOperationError;
        if(started.empty()) {
            clockSequence.fetch_add(1); dsp.reset(); anchor=mach_absolute_time(); seed.fetch_add(1); clockSequence.fetch_add(1); changed=true;
        }
        started.insert(id); running=1;
    }
    if(changed) notify(Device,kAudioDevicePropertyDeviceIsRunning); return noErr;
}
OSStatus stop(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id) {
    if(!deviceOK(d,o)) return kAudioHardwareBadObjectError;
    bool changed=false;
    { std::lock_guard<std::mutex> lock(state); if(!started.erase(id)) return kAudioHardwareIllegalOperationError;
      if(started.empty()) { running=0; changed=true; } }
    if(changed) notify(Device,kAudioDevicePropertyDeviceIsRunning); return noErr;
}
OSStatus zero(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32,Float64 *sample,UInt64 *time,UInt64 *generation) {
    if(!deviceOK(d,o)) return kAudioHardwareBadObjectError;
    if(!sample||!time||!generation) return kAudioHardwareIllegalOperationError;
    auto seq=clockSequence.load(); if(seq&1) return kAudioHardwareNotReadyError;
    const auto origin=anchor.load(),epoch=seed.load(); const auto period=ticksPerFrame.load()*ClockPeriod;
    if(period<=0) return kAudioHardwareNotReadyError;
    const auto now=mach_absolute_time();
    const auto count=uint64_t(double(now>origin?now-origin:0)/period);
    if(clockSequence.load()!=seq) return kAudioHardwareNotReadyError;
    *sample=double(count)*ClockPeriod; *time=origin+uint64_t(double(count)*period); *generation=epoch; return noErr;
}
OSStatus will(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32,UInt32 op,Boolean *yes,Boolean *inPlace) {
    if(!deviceOK(d,o)) return kAudioHardwareBadObjectError;
    if(yes) *yes=op==kAudioServerPlugInIOOperationReadInput||op==kAudioServerPlugInIOOperationWriteMix;
    if(inPlace) *inPlace=true; return noErr;
}
OSStatus beginEnd(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*) { return deviceOK(d,o)?noErr:kAudioHardwareBadObjectError; }
OSStatus io(AudioServerPlugInDriverRef d,AudioObjectID o,AudioObjectID stream,UInt32,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo *cycle,void *main,void*) {
    if(!deviceOK(d,o)) return kAudioHardwareBadObjectError;
    if(!cycle||!main||frames>lcd::MaxFrames) return kAudioHardwareIllegalOperationError;
    bool writing=op==kAudioServerPlugInIOOperationWriteMix;
    if((writing&&stream!=Output)||(!writing&&(op!=kAudioServerPlugInIOOperationReadInput||stream!=Input))) return kAudioHardwareIllegalOperationError;
    const double t=writing?cycle->mOutputTime.mSampleTime:cycle->mInputTime.mSampleTime;
    // Exact integral sample addressing, including negative times at start-up.
    if(!std::isfinite(t)||t!=std::floor(t)||t< -9007199254740991.0||t>9007199254732799.0) return kAudioHardwareIllegalOperationError;
    if(writing) {
        const float master=outputVolume.load();
        const auto hostTime=(cycle->mOutputTime.mFlags&kAudioTimeStampHostTimeValid)?cycle->mOutputTime.mHostTime:0;
        if(outputActive.load()&&!outputMuted.load()&&master>0) dsp.write(int64_t(t),static_cast<const float*>(main),frames,sampleRate.load(),master,hostTime,ticksPerFrame.load());
        // Inactive output must not leave old data at repeated timestamps.
        else dsp.write(int64_t(t),Silence,frames,sampleRate.load(),master,hostTime,ticksPerFrame.load());
    } else {
        if(inputActive.load()) dsp.read(int64_t(t),static_cast<float*>(main),frames);
        else std::memset(main,0,frames*lcd::Channels*sizeof(float));
    }
    return noErr;
}
AudioServerPlugInDriverInterface interface{
    nullptr,query,addRef,release,initialize,createDevice,destroyDevice,client,client,perform,abortChange,
    has,settable,dataSize,get,set,start,stop,zero,will,beginEnd,io,beginEnd
};
AudioServerPlugInDriverInterface *interfacePointer=&interface;
struct Bind { Bind() { driverRef=&interfacePointer; } } bind;
}
extern "C" __attribute__((visibility("default"))) void *PatchlaneDriver_Create(CFAllocatorRef,CFUUIDRef type) {
    return type&&CFEqual(type,kAudioServerPlugInTypeUUID)?driverRef:nullptr;
}
