// Bounded offline callback-cost measurement. Does not open audio devices.
#include "TimedReader.hpp"
#include <memory>
#include <chrono>
#include <vector>
#include <cstdio>
int main() {
    auto ring=std::make_unique<lcshared::AudioRing>();
    lcshared::InterpolationKernel kernel;
    for(int t=0;t<8192;++t) {
        lcshared::Snapshot s;s.generation=1;s.sampleTime=t;
        for(unsigned c=0;c<lcshared::Channels;++c)s.samples[c]=float(std::sin(t*0.15+c));
        ring->write(s);
    }
    ring->publishClock({1,100000,0,8192,1000});
    std::vector<double> durations;
    double sum=0;
    for(unsigned block=0;block<256;++block) {
        auto start=std::chrono::steady_clock::now();
        lcshared::TimedReader reader(*ring,164500+(block%128)*32000,1000,0,&kernel);
        for(unsigned frame=0;frame<32;++frame) {
            float l=0,r=0;if(!reader.read(frame,block%4,l,r))return 1;sum+=l+r;
        }
        auto end=std::chrono::steady_clock::now();
        if(block>=16) durations.push_back(std::chrono::duration<double,std::micro>(end-start).count());
    }
    std::sort(durations.begin(),durations.end());
    std::printf("32-frame callback offline us: median=%.3f p99=%.3f max=%.3f checksum=%.9f\n",durations[durations.size()/2],durations[durations.size()*99/100],durations.back(),sum);
}
