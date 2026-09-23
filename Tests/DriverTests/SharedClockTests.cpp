#include "DriverDSP.hpp"
#include <memory>
#include <limits>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL clock line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)
int main() {
    using namespace lcshared;
    auto ring=std::make_unique<AudioRing>();
    ClockSnapshot clock;
    CHECK(!ring->readClock(clock));
    ring->publishClock({7,100000,-32,0,500});
    CHECK(ring->readClock(clock));
    CHECK(clock.generation==7&&clock.sampleTime==-32&&clock.endTime==0);
    CHECK(clock.hostTime==100000&&clock.ticksPerFrame==500);
    ring->clock.sequence.fetch_add(1);
    CHECK(!ring->readClock(clock));CHECK(clock.generation==0);
    ring->clock.sequence.fetch_add(1);
    ring->invalidateClock();CHECK(!ring->readClock(clock));
    for(double ticks:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        ring->publishClock({7,100000,0,32,ticks});CHECK(!ring->readClock(clock));
    }
    auto dsp=std::make_unique<lcd::DSP>();dsp->attachRing(ring.get());
    float source[32*Channels]{};source[0]=0.25f;
    dsp->write(64,source,32,48000,1,100000,500);
    CHECK(ring->readClock(clock));CHECK(clock.sampleTime==64&&clock.endTime==96);
    Snapshot sample;CHECK(ring->read(64,clock.generation,sample));CHECK(sample.samples[0]==0.25f);
    CHECK(ring->read(95,clock.generation,sample));CHECK(sample.hostTime==115500);
    CHECK(!ring->read(96,clock.generation,sample));
    const auto generation=clock.generation;
    dsp->reset();CHECK(!ring->readClock(clock));
    dsp->write(0,source,32,48000,1,200000,500);
    CHECK(ring->readClock(clock));CHECK(clock.generation!=generation);
    CHECK(!ring->read(64,clock.generation,sample));
    dsp->write(32,source,32,48000);CHECK(!ring->readClock(clock));
    dsp->write(64,source,32,48000,1,300000,500);CHECK(ring->readClock(clock));
    dsp->attachRing(nullptr);CHECK(!ring->readClock(clock));
    std::puts("PASS shared clock: publication, bounds, invalid timestamps, reset and detach");
}
