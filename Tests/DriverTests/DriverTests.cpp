#ifdef PATCHLANE_REALTIME_AUDIT
#include "RealtimeAudit.hpp"
#define AUDIT_CALLBACK(call) { RealtimeAuditScope scope; call; }
#else
#define AUDIT_CALLBACK(call) call
#endif
#include "DriverDSP.hpp"
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <limits>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static std::atomic<UInt64> requested{0};
static OSStatus changed(AudioServerPlugInHostRef,AudioObjectID,UInt32,const AudioObjectPropertyAddress*) { return noErr; }
static OSStatus request(AudioServerPlugInHostRef,AudioObjectID,UInt64 action,void*) { requested=action; return noErr; }
static OSStatus copyStorage(AudioServerPlugInHostRef,CFStringRef,CFPropertyListRef *out) { *out=nullptr; return noErr; }
static OSStatus writeStorage(AudioServerPlugInHostRef,CFStringRef,CFPropertyListRef) { return noErr; }
static OSStatus deleteStorage(AudioServerPlugInHostRef,CFStringRef) { return noErr; }
static void SharedDSPTests() {
    auto dsp=std::make_unique<lcd::DSP>();
    auto ring=std::make_unique<lcshared::AudioRing>();
    dsp->attachRing(ring.get());
    float input[32*8]{};input[7*8]=0.5f;
    dsp->write(100,input,32,48000,0.5f,1000000,500);
    lcshared::Snapshot sample;
    CHECK(ring->read(107,dsp->generation(),sample));
    CHECK(sample.samples[0]==0.25f&&sample.hostTime==1003500);
    const auto generation=dsp->generation();dsp->reset();
    CHECK(!ring->read(107,dsp->generation(),sample));
    CHECK(dsp->generation()!=generation);
    dsp->attachRing(nullptr);dsp->write(100,input,32,48000);
    CHECK(dsp->audioRing().read(107,dsp->generation(),sample));
    CHECK(sample.samples[0]==0.5f);
    std::puts("PASS DSP shared storage: routing output, master gain, host timestamp, reset, detach");
}
static void DSPTests() {
    auto dsp=std::make_unique<lcd::DSP>();
    std::vector<float> input(128*8),output(128*8);
    for(unsigned block:{32u,64u,128u}) {
        dsp->reset(); std::fill(input.begin(),input.end(),0);
        for(unsigned c=0;c<8;++c) input[14*8+c]=(c+1)*0.05f;
        // The first callback block is available at the SAME sample timestamp.
        dsp->write(-16,input.data(),block,44100); dsp->read(-16,output.data(),block);
        for(unsigned i=0;i<block*8;++i) CHECK(input[i]==output[i]);
        CHECK(dsp->missing==0);
        dsp->read(10000,output.data(),block);
        for(unsigned i=0;i<block*8;++i) CHECK(output[i]==0);
        CHECK(dsp->missing==block);
        // Read granularity may differ from the producer's callback granularity.
        dsp->read(-16,output.data(),16); dsp->read(0,output.data()+16*8,block-16);
        for(unsigned i=0;i<block*8;++i) CHECK(input[i]==output[i]);
    }
    lcd::Config config;
    for(auto &r:config.route) for(auto &v:r) v=0;
    config.route[0][3]=1; config.route[2][3]=1; config.inputGain[0]=0.5;
    config.outputGain[3]=0.5;
    CHECK(dsp->controls.publish(config)); dsp->reset();
    std::fill(input.begin(),input.end(),0);
    input[0]=0.4; input[1]=-0.4; input[4]=0.6; input[5]=-0.6;
    dsp->write(0,input.data(),32,48000); dsp->read(0,output.data(),32);
    CHECK(std::abs(output[6]-0.4f)<1e-6); CHECK(std::abs(output[7]+0.4f)<1e-6);
    for(unsigned c=0;c<6;++c) CHECK(output[c]==0);
    // Same ring slot, different timestamp must never replay old audio.
    dsp->write(lcd::Capacity,input.data(),32,48000); dsp->read(0,output.data(),32);
    for(unsigned i=0;i<32*8;++i) CHECK(output[i]==0);
    dsp->reset(); dsp->read(lcd::Capacity,output.data(),32);
    for(unsigned i=0;i<32*8;++i) CHECK(output[i]==0);
    config.version=0; CHECK(!dsp->controls.publish(config)); config.version=1;
    config.inputGain[0]=std::numeric_limits<float>::quiet_NaN(); CHECK(!dsp->controls.publish(config));
    config=lcd::Config{}; CHECK(dsp->controls.publish(config)); dsp->reset();
    input[0]=std::numeric_limits<float>::infinity(); input[1]=2;
    dsp->write(0,input.data(),32,44100); dsp->read(0,output.data(),32);
    CHECK(output[0]==0&&output[1]==1);
    // Concurrent readers and a lapping producer. A conflict may yield silence,
    // but must never yield another timestamp's sample or a torn frame.
    dsp->reset(); std::atomic<bool> done{false}; std::atomic<int64_t> latest{-1};
    auto signal=[](int64_t t,unsigned c) { return float((uint64_t(t)*13+c)%997+1)/1000.f; };
    auto consume=[&] {
        float block[32*8];
        while(!done.load()) {
            auto t=latest.load(); if(t<0) continue;
            dsp->read(t,block,32);
            for(unsigned f=0;f<32;++f) {
                const bool silent=block[f*8]==0;
                for(unsigned c=0;c<8;++c) CHECK(block[f*8+c]==(silent?0:signal(t+f,c)));
            }
        }
    };
    std::thread a(consume),b(consume);
    for(int64_t t=0;t<lcd::Capacity*4;t+=32) {
        for(unsigned f=0;f<32;++f) for(unsigned c=0;c<8;++c) input[f*8+c]=signal(t+f,c);
        dsp->write(t,input.data(),32,44100); latest=t;
    }
    done=true; a.join(); b.join();
    std::puts("PASS DSP: first block, zero sample shift, 4x4 mix, missing data, wrap, restart, NaN, concurrent readers");
}
int main(int argc,char **argv) {
    CHECK(argc==2||argc==3);
    const bool propertiesOnly=argc==3&&std::strcmp(argv[2],"--properties-only")==0;
    const bool stereo=argc==3&&std::strcmp(argv[2],"--stereo")==0;
    CHECK(argc==2||propertiesOnly||stereo);
    const unsigned channelCount=stereo?2:8;
    if(!propertiesOnly) { SharedDSPTests(); DSPTests(); }
    auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(argv[1]),std::strlen(argv[1]),true);
    CHECK(url); auto bundle=CFBundleCreate(nullptr,url); CFRelease(url); CHECK(bundle);
    CHECK(CFBundleLoadExecutable(bundle));
    using Factory=void*(*)(CFAllocatorRef,CFUUIDRef);
    auto factory=reinterpret_cast<Factory>(CFBundleGetFunctionPointerForName(bundle,CFSTR("PatchlaneDriver_Create"))); CHECK(factory);
    CHECK(factory(nullptr,IUnknownUUID)==nullptr);
    auto driver=static_cast<AudioServerPlugInDriverRef>(factory(nullptr,kAudioServerPlugInTypeUUID)); CHECK(driver);
    auto api=*driver;
    LPVOID queried=nullptr;
    CHECK(api->QueryInterface(driver,CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID),&queried)==0&&queried==driver);
    api->Release(driver);
    AudioServerPlugInHostInterface host{changed,copyStorage,writeStorage,deleteStorage,request};
    CHECK(api->Initialize(driver,&host)==0);
    auto address=[](UInt32 key,UInt32 scope=kAudioObjectPropertyScopeGlobal) { return AudioObjectPropertyAddress{key,scope,kAudioObjectPropertyElementMain}; };
    auto get=[&](AudioObjectID id,UInt32 key,UInt32 size,void *out,UInt32 scope=kAudioObjectPropertyScopeGlobal) {
        auto p=address(key,scope); UInt32 used=0; CHECK(api->GetPropertyData(driver,id,0,&p,0,nullptr,size,&used,out)==0); CHECK(used==size);
    };
    auto deviceList=address(kAudioPlugInPropertyDeviceList); UInt32 size=0;
    CHECK(api->GetPropertyDataSize(driver,1,0,&deviceList,0,nullptr,&size)==0&&size==4);
    AudioObjectID device=0; get(1,kAudioPlugInPropertyDeviceList,4,&device); CHECK(device==2);
    CFDataRef sharedStatus=nullptr;get(device,'lshm',sizeof(sharedStatus),&sharedStatus);
    CHECK(sharedStatus&&CFDataGetLength(sharedStatus)==6*sizeof(uint64_t));
    uint64_t diagnostic[6];std::memcpy(diagnostic,CFDataGetBytePtr(sharedStatus),sizeof(diagnostic));CFRelease(sharedStatus);
    CHECK(diagnostic[0]==1&&diagnostic[1]==lcshared::Version);
    CHECK(diagnostic[2]>=sizeof(lcshared::AudioRing)&&diagnostic[3]==KERN_SUCCESS);
    Boolean statusWritable=true;auto sharedAddress=address('lshm');
    CHECK(api->IsPropertySettable(driver,device,0,&sharedAddress,&statusWritable)==0&&!statusWritable);
    if(propertiesOnly) { std::puts("PASS shared diagnostics: CFData wire format, ABI, mapping allocation, read-only property");return 0; }
    auto controlList=address(kAudioObjectPropertyControlList);
    UInt32 controlBytes=0;
    CHECK(api->GetPropertyDataSize(driver,device,0,&controlList,0,nullptr,&controlBytes)==0);
    CHECK(controlBytes==2*sizeof(AudioObjectID)); // macOS needs standard volume + mute controls.
    AudioObjectID controls[2]; get(device,kAudioObjectPropertyControlList,sizeof(controls),controls);
    UInt32 controlClass=0,controlScope=0,controlElement=99;
    get(controls[0],kAudioObjectPropertyClass,4,&controlClass); CHECK(controlClass==kAudioVolumeControlClassID);
    get(controls[0],kAudioControlPropertyScope,4,&controlScope); CHECK(controlScope==kAudioObjectPropertyScopeOutput);
    get(controls[0],kAudioControlPropertyElement,4,&controlElement); CHECK(controlElement==kAudioObjectPropertyElementMain);
    get(controls[1],kAudioObjectPropertyClass,4,&controlClass); CHECK(controlClass==kAudioMuteControlClassID);
    auto volumeAddress=address(kAudioLevelControlPropertyScalarValue);
    Boolean volumeWritable=false;
    CHECK(api->IsPropertySettable(driver,controls[0],0,&volumeAddress,&volumeWritable)==0&&volumeWritable);
    AudioObjectID streams[2]; get(device,kAudioDevicePropertyStreams,sizeof(streams),streams); CHECK(streams[0]==3&&streams[1]==4);
    AudioStreamBasicDescription fmt{}; get(3,kAudioStreamPropertyVirtualFormat,sizeof(fmt),&fmt);
    CHECK(fmt.mChannelsPerFrame==channelCount&&fmt.mBytesPerFrame==channelCount*4&&fmt.mSampleRate==44100);
    CFStringRef uid=nullptr;get(device,kAudioDevicePropertyDeviceUID,sizeof(uid),&uid);
    CHECK(CFEqual(uid,stereo?CFSTR("audio.patchlane.virtual2"):CFSTR("audio.patchlane.virtual8")));CFRelease(uid);
    if(stereo) {
        struct { AudioChannelLayoutTag tag; AudioChannelBitmap bitmap; UInt32 count; AudioChannelDescription channels[2]; } layout{};
        get(device,kAudioDevicePropertyPreferredChannelLayout,sizeof(layout),&layout,kAudioObjectPropertyScopeInput);
        CHECK(layout.count==2 && layout.channels[0].mChannelLabel==kAudioChannelLabel_Left && layout.channels[1].mChannelLabel==kAudioChannelLabel_Right);
        AudioStreamBasicDescription outputFormat{};get(4,kAudioStreamPropertyVirtualFormat,sizeof(outputFormat),&outputFormat);
        CHECK(outputFormat.mChannelsPerFrame==2 && outputFormat.mBytesPerFrame==8);
    }
    UInt32 latency=999; get(device,kAudioDevicePropertyLatency,4,&latency,kAudioObjectPropertyScopeInput); CHECK(latency==0);
    auto name=address(kAudioObjectPropertyName); char small[2]; UInt32 used=0;
    CHECK(api->GetPropertyData(driver,device,0,&name,0,nullptr,2,&used,small)==kAudioHardwareBadPropertySizeError);
    CHECK(!api->HasProperty(driver,999,0,&name));
    lcd::Config config;
    config.route[0][1]=1; config.outputGain[1]=0.25;
    auto blob=CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(&config),sizeof(config)); auto cfg=address('lcfg');
    CHECK(api->SetPropertyData(driver,device,0,&cfg,0,nullptr,sizeof(blob),&blob)==0); CFRelease(blob);
    CFDataRef readback=nullptr; get(device,'lcfg',sizeof(readback),&readback);
    CHECK(CFDataGetLength(readback)==sizeof(config)&&std::memcmp(CFDataGetBytePtr(readback),&config,sizeof(config))==0); CFRelease(readback);
    CHECK(api->StartIO(driver,device,11)==0); CHECK(api->StartIO(driver,device,12)==0);
    CHECK(api->StartIO(driver,device,11)!=0);
    double sample; UInt64 time,seed;
    AUDIT_CALLBACK(CHECK(api->GetZeroTimeStamp(driver,device,11,&sample,&time,&seed)==0&&time>0);)
    const auto initialSeed=seed;
    Boolean will=false,inPlace=false;
    AUDIT_CALLBACK(CHECK(api->WillDoIOOperation(driver,device,11,kAudioServerPlugInIOOperationWriteMix,&will,&inPlace)==0&&will&&inPlace);)
    std::vector<float> input(32*channelCount),output(32*channelCount); input[7*channelCount]=0.5; input[7*channelCount+1]=-0.5;
    AudioServerPlugInIOCycleInfo cycle{};
    cycle.mInputTime.mSampleTime=cycle.mOutputTime.mSampleTime=512;
    AUDIT_CALLBACK(CHECK(api->DoIOOperation(driver,device,4,11,kAudioServerPlugInIOOperationWriteMix,32,&cycle,input.data(),nullptr)==0);)
    AUDIT_CALLBACK(CHECK(api->DoIOOperation(driver,device,3,12,kAudioServerPlugInIOOperationReadInput,32,&cycle,output.data(),nullptr)==0);)
    for(unsigned f=0;f<32;++f) for(unsigned c=0;c<channelCount;++c) {
        float expected=f==7?(c==0?0.5f:c==1?-0.5f:c==2?0.125f:c==3?-0.125f:0):0;
        CHECK(output[f*channelCount+c]==expected);
    }
    auto setVolume=[&](float v) { CHECK(api->SetPropertyData(driver,controls[0],0,&volumeAddress,0,nullptr,sizeof(v),&v)==0); };
    auto process=[&] {
        cycle.mInputTime.mSampleTime+=32; cycle.mOutputTime.mSampleTime+=32;
    AUDIT_CALLBACK(CHECK(api->DoIOOperation(driver,device,4,11,kAudioServerPlugInIOOperationWriteMix,32,&cycle,input.data(),nullptr)==0);)
    AUDIT_CALLBACK(CHECK(api->DoIOOperation(driver,device,3,12,kAudioServerPlugInIOOperationReadInput,32,&cycle,output.data(),nullptr)==0);)
    };
    setVolume(0.5f);
    float level=0; get(controls[0],kAudioLevelControlPropertyScalarValue,4,&level); CHECK(level==0.5f);
    for(int n=0;n<120;++n) process(); // Let amplitude dezippering settle, no sample delay.
    for(unsigned f=0;f<32;++f) for(unsigned c=0;c<channelCount;++c) {
        float expected=f==7?(c==0?0.25f:c==1?-0.25f:c==2?0.0625f:c==3?-0.0625f:0):0;
        CHECK(std::abs(output[f*channelCount+c]-expected)<1e-5f);
    }
    float db=0; get(controls[0],kAudioLevelControlPropertyDecibelValue,4,&db); CHECK(std::abs(db+6.0206f)<0.001f);
    float converted=0.5f; get(controls[0],kAudioLevelControlPropertyConvertScalarToDecibels,4,&converted); CHECK(std::abs(converted-db)<0.001f);
    get(controls[0],kAudioLevelControlPropertyConvertDecibelsToScalar,4,&converted); CHECK(std::abs(converted-0.5f)<1e-6f);
    auto muteAddress=address(kAudioBooleanControlPropertyValue); UInt32 mute=1;
    CHECK(api->SetPropertyData(driver,controls[1],0,&muteAddress,0,nullptr,4,&mute)==0);
    process(); for(auto v:output) CHECK(v==0);
    get(controls[0],kAudioLevelControlPropertyScalarValue,4,&level); CHECK(level==0.5f); // Mute preserves volume.
    mute=0; CHECK(api->SetPropertyData(driver,controls[1],0,&muteAddress,0,nullptr,4,&mute)==0);
    process(); CHECK(std::abs(output[7*channelCount]-0.25f)<1e-5f);
    setVolume(0); process(); for(auto v:output) CHECK(v==0);
    float invalid=std::numeric_limits<float>::quiet_NaN();
    CHECK(api->SetPropertyData(driver,controls[0],0,&volumeAddress,0,nullptr,4,&invalid)!=0);
    setVolume(1);
    std::puts("PASS macOS volume controls: discovery, writable scalar, dB conversion, real attenuation, mute/unmute, zero, unchanged impulse position");
    CHECK(api->StopIO(driver,device,11)==0);
    UInt32 active=0; get(device,kAudioDevicePropertyDeviceIsRunning,4,&active); CHECK(active==1);
    CHECK(api->StopIO(driver,device,12)==0); get(device,kAudioDevicePropertyDeviceIsRunning,4,&active); CHECK(active==0);
    auto rateAddress=address(kAudioDevicePropertyNominalSampleRate); double rate=48000;
    CHECK(api->StartIO(driver,device,11)==0);
    CHECK(api->SetPropertyData(driver,device,0,&rateAddress,0,nullptr,sizeof(rate),&rate)==0);
    for(int i=0;i<1000&&!requested.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(requested==48000);
    CHECK(api->PerformDeviceConfigurationChange(driver,device,48000,nullptr)!=0);
    CHECK(api->StopIO(driver,device,11)==0);
    requested=0; // A failed perform must not swallow the next request.

    CHECK(api->SetPropertyData(driver,device,0,&rateAddress,0,nullptr,sizeof(rate),&rate)==0);
    for(int i=0;i<1000&&!requested.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(requested==48000); CHECK(api->PerformDeviceConfigurationChange(driver,device,48000,nullptr)==0);
    get(device,kAudioDevicePropertyNominalSampleRate,sizeof(rate),&rate); CHECK(rate==48000);
    CHECK(api->StartIO(driver,device,11)==0);
    AUDIT_CALLBACK(CHECK(api->GetZeroTimeStamp(driver,device,11,&sample,&time,&seed)==0&&seed>initialSeed);)
    AUDIT_CALLBACK(CHECK(api->DoIOOperation(driver,device,3,11,kAudioServerPlugInIOOperationReadInput,32,&cycle,output.data(),nullptr)==0);)
    for(auto v:output) CHECK(v==0); // No audio survives a restart/rate change.
    CHECK(api->StopIO(driver,device,11)==0);
    std::puts("PASS bundle: factory, interface, discovery, channel formats, control roundtrip, timestamped IO, clients, rate change, restart");
    // Keep the bundle loaded until process exit: it contains dispatch callbacks.
    CFRelease(bundle);
}
