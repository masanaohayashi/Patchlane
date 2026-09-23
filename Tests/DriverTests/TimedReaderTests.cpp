#include "TimedReader.hpp"
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <limits>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL timed reader line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)
int main() {
    using namespace lcshared;
    auto ring=std::make_unique<AudioRing>();
    const uint64_t host=(uint64_t(1)<<60)+123;
    // Negative sample times, huge host uptime and all four stereo buses.
    for(int64_t t=-32;t<32;++t) {
        Snapshot s;s.generation=9;s.sampleTime=t;s.hostTime=host+uint64_t((t+32)*500);
        for(unsigned c=0;c<Channels;++c) s.samples[c]=float(t)+float(c)/8;
        ring->write(s);
    }
    ring->publishClock({9,host,-32,32,500});
    float l=99,r=99;
    TimedReader exact(*ring,host,500,0);
    for(unsigned i=0;i<64;++i) for(unsigned bus=0;bus<4;++bus) {
        CHECK(exact.read(i,bus,l,r));CHECK(l==float(int(i)-32)+float(bus)/4);CHECK(r==l+0.125f);
    }
    CHECK(!exact.read(64,0,l,r));CHECK(l==0&&r==0);
    TimedReader half(*ring,host+250,500,0);
    CHECK(half.read(0,0,l,r));CHECK(l==-31.5f);
    CHECK(!half.read(63,0,l,r));CHECK(l==0&&r==0);
    TimedReader delayed(*ring,host+5000,500,7);
    CHECK(delayed.read(0,0,l,r));CHECK(l==-29);
    TimedReader skew(*ring,host,500.5,0);
    CHECK(skew.read(20,0,l,r));CHECK(std::abs(l-(-11.98f))<0.00001f);
    TimedReader before(*ring,host-250,500,0);
    CHECK(!before.read(0,0,l,r));
    for(double delay:{-1.,double(Capacity),std::numeric_limits<double>::quiet_NaN()}) {
        TimedReader invalid(*ring,host,500,delay);CHECK(!invalid.read(0,0,l,r));
    }
    // Producer completes a frame after this callback captured its clock.
    // Its own timestamp/generation/seqlock prove readiness without waiting for
    // the next block clock publication or retrying a read.
    Snapshot arrived;arrived.generation=9;arrived.sampleTime=32;arrived.hostTime=host+32000;
    arrived.samples[0]=32;arrived.samples[1]=32.125f;ring->write(arrived);
    CHECK(exact.read(64,0,l,r));CHECK(l==32&&r==32.125f);
    CHECK(half.read(63,0,l,r));CHECK(l==31.5f);
    ring->frames[32].sequence.fetch_add(1);
    CHECK(!exact.read(64,0,l,r));CHECK(!half.read(63,0,l,r));
    ring->frames[32].sequence.fetch_add(1);
    ring->invalidateClock();CHECK(!exact.read(0,0,l,r));CHECK(l==0&&r==0);
    ring->publishClock({10,host,-32,32,500});
    TimedReader restarted(*ring,host,500,0);CHECK(!restarted.read(0,0,l,r));
    std::puts("PASS timed reader: exact/fractional time, explicit delay, clock skew, bounds, generation");
}
