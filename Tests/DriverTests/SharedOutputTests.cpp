#ifdef PATCHLANE_REALTIME_AUDIT
#include "RealtimeAudit.hpp"
#define AUDIT_CALLBACK(call) { RealtimeAuditScope scope; call; }
#else
#define AUDIT_CALLBACK(call) call
#endif
// Exercise the real IOProc with synthetic buffers, without opening a device.
#include "../../Sources/AudioCore/DeviceBridge.cpp"
#include <cstdlib>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL output line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)
int main() {
    LCSharedOutput output;
    CHECK(output.mapping.create()==KERN_SUCCESS);
    auto *ring=output.mapping.producer();
    for(int64_t t=0;t<32;++t) {
        lcshared::Snapshot sample;sample.generation=1;sample.sampleTime=t;sample.hostTime=10000+uint64_t(t)*500;
        sample.samples[2]=float(t)/64;sample.samples[3]=-float(t)/64;ring->write(sample);
    }
    ring->publishClock({1,10000,0,32,500});
    output.bus=1;output.left=1;output.right=3;output.nominalTicks=500;
    float samples[32*4];std::fill(std::begin(samples),std::end(samples),99);
    AudioBufferList buffers{};buffers.mNumberBuffers=1;buffers.mBuffers[0]={4,sizeof(samples),samples};
    AudioTimeStamp time{};time.mHostTime=10000;time.mRateScalar=1;
    time.mFlags=kAudioTimeStampHostTimeValid|kAudioTimeStampRateScalarValid;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&output));
    CHECK(output.callbacks==1&&output.missing==0);
    for(unsigned f=0;f<32;++f) {
        CHECK(samples[f*4]==0&&samples[f*4+2]==0);
        CHECK(samples[f*4+1]==float(f)/64&&samples[f*4+3]==-float(f)/64);
    }
    time.mHostTime=10000+32*500;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&output));
    CHECK(output.missing==32);for(float v:samples) CHECK(v==0);
    output.delay=32;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&output));
    CHECK(output.missing==32);CHECK(samples[31*4+1]==31.f/64);
    time.mFlags=kAudioTimeStampHostTimeValid;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&output));
    CHECK(output.missing==64);for(float v:samples) CHECK(v==0);
    CHECK(output.received==64&&output.initialMissing==0);
    LCSharedOutput waiting;CHECK(waiting.mapping.create()==KERN_SUCCESS);waiting.nominalTicks=500;
    time.mHostTime=10000;time.mFlags=kAudioTimeStampHostTimeValid|kAudioTimeStampRateScalarValid;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&waiting));
    CHECK(waiting.missing==32&&waiting.initialMissing==32&&waiting.received==0);
    auto *waitingRing=waiting.mapping.producer();
    for(int64_t t=0;t<32;++t) {
        lcshared::Snapshot sample;sample.generation=1;sample.sampleTime=t;
        waitingRing->write(sample); // Valid silence also ends the initial wait.
    }
    waitingRing->publishClock({1,10000,0,32,500});
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&waiting));
    CHECK(waiting.received==32&&waiting.initialMissing==32&&waiting.missing==32);
    time.mHostTime+=16000;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&waiting));
    CHECK(waiting.received==32&&waiting.initialMissing==32&&waiting.missing==64);
    // Fractional physical clock uses the actual production sinc path.
    LCSharedOutput fractional;CHECK(fractional.mapping.create()==KERN_SUCCESS);fractional.nominalTicks=500;
    auto *fractionalRing=fractional.mapping.producer();
    for(int t=-64;t<128;++t) {
        lcshared::Snapshot sample;sample.generation=1;sample.sampleTime=t;
        sample.samples[0]=sample.samples[1]=float(std::sin(2*3.141592653589793*20000*t/48000));fractionalRing->write(sample);
    }
    fractionalRing->publishClock({1,10000,0,128,500});
    time.mHostTime=10250;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&fractional));
    CHECK(fractional.missing==0);
    for(unsigned f=0;f<32;++f) CHECK(std::abs(samples[f*4]-std::sin(2*3.141592653589793*20000*(f+0.5)/48000))<0.001);
    // Missing lookahead must never fall back to attenuating linear interpolation.
    fractionalRing->frames[fractional.interpolation.after].generation.store(0);
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&fractional));
    CHECK(fractional.missing==32);for(float v:samples) CHECK(v==0);
    CHECK(!lc_shared_output_needs_reconnect(&fractional));
    fractionalRing->publishClock({1,10000,0,128,250}); // Source nominal rate doubles.
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&fractional));
    CHECK(lc_shared_output_needs_reconnect(&fractional));
    for(float v:samples) CHECK(v==0);
    LCSharedOutput identity;CHECK(identity.mapping.create()==KERN_SUCCESS);
    lcshared::MappedAudioRing same,replacement;
    CHECK(same.mapReadOnly(identity.mapping.readOnlyPort(),identity.mapping.mappedSize())==KERN_SUCCESS);
    const auto *original=identity.mapping.view();
    identity.observeSource(same);CHECK(!lc_shared_output_needs_reconnect(&identity));
    CHECK(replacement.create()==KERN_SUCCESS);
    identity.observeSource(replacement);CHECK(lc_shared_output_needs_reconnect(&identity));
    CHECK(identity.mapping.view()==original); // Live IOProc mapping was not replaced.
    // The shared screen can select any L/R source channels and fan one
    // stereo input out to independent physical outputs. Meters measure samples.
    LCSharedOutput routed;
    CHECK(routed.mapping.mapReadOnly(output.mapping.readOnlyPort(),output.mapping.mappedSize())==KERN_SUCCESS);
    routed.nominalTicks=500;routed.left=0;routed.right=1;
    CHECK(lc_shared_output_channels(&routed,3,2)==0);
    lc_shared_output_levels(&routed,0.5f,1,0);
    routed.currentInput=0.5f;
    time.mHostTime=10000;time.mRateScalar=1;
    time.mFlags=kAudioTimeStampHostTimeValid|kAudioTimeStampRateScalarValid;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&routed));
    CHECK(std::abs(samples[31*4]+31.f/128)<0.00001f);
    CHECK(std::abs(samples[31*4+1]-31.f/128)<0.00001f);
    CHECK(lc_shared_output_input_peak(&routed)>0.24f);
    CHECK(lc_shared_output_peak(&routed)>0.24f);
    CHECK(lc_shared_output_peak(&routed)==0);
    lc_shared_output_levels(&routed,0.5f,0,0);routed.currentRoute=0;
    AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&routed));
    CHECK(lc_shared_output_input_peak(&routed)>0.24f);
    CHECK(lc_shared_output_peak(&routed)==0);
    for(float value:samples) CHECK(value==0);
    LCDipoleConfig dipole{};dipole.spacingCM=20;dipole.distanceCM=100;dipole.headCM=17.5;dipole.maxBoostDB=12;dipole.tonalReference=1;
    dipole.geqDB[0]=12;dipole.geqDB[17] = -6;
    CHECK(lc_shared_output_dipole(&routed,&dipole,44100)==0);lc_shared_output_dipole_enabled(&routed,1);
    for(int block=0;block<300;++block) { AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&routed)); }
    lc_shared_output_dipole_enabled(&routed,0);
    for(int block=0;block<20;++block) { AUDIT_CALLBACK(LCSharedOutput::callback(0,nullptr,nullptr,nullptr,&buffers,&time,&routed)); }
    std::puts("PASS shared controls: selected L/R, gain, route off, independent input/output meters");
    std::puts("PASS shared output callback: channel routing, zero delay, missing frames, explicit delay, invalid clock");
}
