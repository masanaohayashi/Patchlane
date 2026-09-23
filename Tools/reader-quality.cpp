// Offline measurement through the production TimedReader, no audio devices.
#include "TimedReader.hpp"
#include <memory>
#include <cstdio>
int main(int argc,char**) {
    lcshared::InterpolationKernel kernel;
    auto ring=std::make_unique<lcshared::AudioRing>();
    constexpr unsigned count=8192;
    constexpr double pi=3.141592653589793;
    bool pass=true;
    for(double rate:{44100.,48000.}) for(double frequency:{1000.,5000.,10000.,20000.}) {
        for(unsigned i=0;i<count+128;++i) {
            lcshared::Snapshot sample;sample.generation=1;sample.sampleTime=i;sample.hostTime=100000+i*1000;
            sample.samples[0]=sample.samples[1]=std::sin(2*pi*frequency*i/rate);ring->write(sample);
        }
        ring->publishClock({1,100000,0,count+128,1000});
        lcshared::TimedReader reader(*ring,164500,1000,0,argc>1?nullptr:&kernel);
        double expectedEnergy=0,outputEnergy=0;
        for(unsigned i=0;i<count;++i) {
            float l=0,r=0;if(!reader.read(i,0,l,r)) return 2;
            double expected=std::sin(2*pi*frequency*(i+64.5)/rate);
            expectedEnergy+=expected*expected;outputEnergy+=double(l)*l;
        }
        double db=10*std::log10(outputEnergy/expectedEnergy);
        std::printf("rate=%.0f frequency=%.0f half_sample_gain_db=%.6f\n",rate,frequency,db);
        if(std::abs(db)>0.1) pass=false;
    }
    std::puts(pass?"PASS within 0.1 dB":"FAIL exceeds 0.1 dB frequency response target");return pass?0:1;
}
