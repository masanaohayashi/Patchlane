#ifdef PATCHLANE_REALTIME_AUDIT
#include "RealtimeAudit.hpp"
#define AUDIT_CALLBACK(call) { RealtimeAuditScope scope; call; }
#else
#define AUDIT_CALLBACK(call) call
#endif
#define LCD_CHANNELS 2
#include "../../Driver/DriverDSP.hpp"
#include "../../Sources/AudioCore/DeviceBridge.cpp"
#include <memory>
#include <cassert>
int main() {
    auto dsp=std::make_unique<lcd::DSP>();
    lcshared::MappedAudioRing producer;
    assert(producer.create()==KERN_SUCCESS);
    LCSharedOutput output;
    assert(output.mapping.mapReadOnly(producer.readOnlyPort(),producer.mappedSize())==KERN_SUCCESS);
    dsp->attachRing(producer.producer());
    float source[256];
    for(unsigned f=0;f<128;++f) { source[2*f]=0.125f;source[2*f+1]=-0.25f; }
    dsp->write(0,source,128,44100,1,10000,500);
    output.nominalTicks=500;
    float dest[64]{};
    AudioBufferList buffers{};buffers.mNumberBuffers=1;buffers.mBuffers[0]={2,sizeof(dest),dest};
    AudioTimeStamp when{};when.mHostTime=10000+32*500;when.mRateScalar=1;
    when.mFlags=kAudioTimeStampHostTimeValid|kAudioTimeStampRateScalarValid;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&when,&output));
    assert(output.received==32 && output.missing==0);
    for(unsigned f=0;f<32;++f) { assert(std::abs(dest[2*f]-0.125f)<0.0001f);assert(std::abs(dest[2*f+1]+0.25f)<0.0001f); }
    dsp->attachRing(nullptr);
    puts("PASS stereo: production 2ch DSP -> read-only shared mapping -> output IOProc, independent L/R, no missing frames");
}
