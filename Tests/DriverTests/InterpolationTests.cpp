#include "TimedReader.hpp"
#include <memory>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL interpolation line %d\n",__LINE__);return 1;}}while(0)
int main() {
    constexpr double pi=3.141592653589793;
    constexpr unsigned frames=2048;
    auto ring=std::make_unique<lcshared::AudioRing>();
    for(double sourceRate:{44100.,48000.,88200.,96000.}) for(double outputRate:{44100.,48000.}) {
        const double step=sourceRate/outputRate;
        lcshared::InterpolationKernel kernel(step);
        for(double frequency:{1000.,10000.,20000.,30000.}) {
            if(frequency>=sourceRate/2) continue;
            for(unsigned i=0;i<8192;++i) {
                lcshared::Snapshot sample;sample.generation=1;sample.sampleTime=i;
                sample.samples[0]=float(std::sin(2*pi*frequency*i/sourceRate));ring->write(sample);
            }
            ring->publishClock({1,100000,0,8192,1000});
            for(double phase:{0.01,0.25,0.5,0.99}) {
                lcshared::TimedReader reader(*ring,228000+uint64_t(phase*1000),1000*step,0,&kernel);
                double energy=0,reference=0;
                for(unsigned i=0;i<frames;++i) {
                    float l=0,r=0;CHECK(reader.read(i,0,l,r));energy+=double(l)*l;
                    const double x=std::sin(2*pi*frequency*(128+phase+i*step)/sourceRate);reference+=x*x;
                }
                const double db=10*std::log10(energy/reference);
                if((frequency<=20000&&std::abs(db)>=0.1)||(frequency>20000&&db>=-50)) std::fprintf(stderr,"source=%g output=%g tone=%g phase=%g gain=%g dB\n",sourceRate,outputRate,frequency,phase,db);
                if(frequency<=20000) CHECK(std::abs(db)<0.1);
                else CHECK(db< -50);
            }
        }
    }
    std::puts("PASS interpolation: 44.1/48/88.2/96 kHz rate pairs, phase offsets, passband and alias suppression");
}
