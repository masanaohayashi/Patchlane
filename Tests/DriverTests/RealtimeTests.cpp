#include <AudioToolbox/AudioToolbox.h>
// The OS capture call is replaced only in this fixture; the real capture
// callback's sample extraction, ring publication and metering run unchanged.
static OSStatus fixtureRender(AudioUnit,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList *data) {
    auto *samples=static_cast<float*>(data->mBuffers[0].mData);
    for(UInt32 i=0;i<frames*2;++i) samples[i]=0.25f;
    return noErr;
}
#define AudioUnitRender fixtureRender
#include "../../Sources/AudioCore/AudioCore.cpp"
#undef AudioUnitRender
#include "../../Driver/DriverDSP.hpp"
#include "RealtimeAudit.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <thread>

int main() {
    // Positive control: the checker must actually intercept a forbidden call.
    auto child=fork();assert(child>=0);
    if(child==0) {
        pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
        RealtimeAuditScope scope;
        pthread_mutex_lock(&mutex);
        _exit(1);
    }
    int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==86);
    child=fork();assert(child>=0);
    if(child==0) {
        void *(*volatile allocate)(size_t)=malloc;
        RealtimeAuditScope scope;
        (void)allocate(16);
        _exit(1);
    }
    assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==86);
    auto engine=std::make_unique<LCEngine>();
    engine->input[0].capture.resize(MaxFrames*2);
    auto dsp=std::make_unique<lcd::DSP>();
    float stereo[64],virtualAudio[32*lcd::Channels],destination[32*lcd::Channels];
    std::fill(std::begin(stereo),std::end(stereo),0.25f);
    std::fill(std::begin(virtualAudio),std::end(virtualAudio),0.25f);
    engine->input[0].route[0]=true;
    AudioBufferList buffers{};buffers.mNumberBuffers=1;buffers.mBuffers[0]={2,sizeof(stereo),stereo};
    // Control publication and UI meter reads must not force audio to wait.
    std::thread control([&] {
        for(int i=0;i<256;++i) {
            lcd::Config config;config.inputGain[0]=i%2 ? 0.5f:1.f;
            assert(dsp->controls.publish(config));
            lc_input_gain(engine.get(),0,config.inputGain[0]);
            (void)lc_output_peak(engine.get(),0);
        }
    });
    for(int i=0;i<256;++i) {
        RealtimeAuditScope scope;
        assert(capture(&engine->input[0],nullptr,nullptr,0,32,nullptr)==noErr);
        assert(render(&engine->contexts[0],nullptr,nullptr,0,32,&buffers)==noErr);
        dsp->write(i*32,virtualAudio,32,44100);
        dsp->read(i*32,destination,32);
    }
    control.join();
    assert(dsp->missing==0);
    assert(destination[0]>0);
    std::puts("PASS: normal capture/render and driver DSP: no intercepted locks, allocation or waits; concurrent control updates");
}
