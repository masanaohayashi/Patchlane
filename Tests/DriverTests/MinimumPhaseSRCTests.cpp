#include "../../Sources/AudioCore/MinimumPhaseSRC.hpp"
#include <cassert>
#include <cstdio>
#include <complex>
#include <cstdint>
int main(){
    constexpr double pi=3.14159265358979323846;
    for(auto rates:{std::pair<double,double>{44100,48000},{48000,44100},{96000,44100},{48000,48000}}){
        const double step=rates.first/rates.second;
        patchsrc::MinimumPhaseKernel k(step);
        // DC, stereo isolation, and polyphase boundary continuity.
        double worst=0;
        for(int p=0;p<256;++p){float l,r;k.sample(1000+p/256.,[](uint64_t,int c){return c?0.f:1.f;},l,r);assert(std::abs(l-1)<2e-6&&r==0);}
        double lowDelay=0;for(int j=0;j<k.taps;++j)lowDelay+=j*k.coefficients[j];
        assert(lowDelay>0&&lowDelay<12*std::max(1.,step));
        for(double hz:{1000.,10000.,18000.,std::min(rates.first,rates.second)*.499}){
            if(hz>=rates.first/2)continue;
            double power=0;
            for(int n=0;n<4096;++n){float l,r;
                k.sample(2000+n*step,[&](uint64_t i,int c){return c?0.f:float(std::sin(2*pi*hz*i/rates.first));},l,r);power+=l*l;assert(r==0);}
            const double db=10*std::log10(power/2048);
            std::printf("%.0f -> %.0f: %.0f Hz %.3f dB\n",rates.first,rates.second,hz,db);
            if(hz<=18000)assert(std::abs(db)<.1);else assert(db < -65);
        }
        // Sweep the complete alias band, not just a conveniently placed zero.
        if(step>1){
            double maximum=0;
            for(int f=0;f<=512;++f){
                const double frequency=(.5/step)+(.5-.5/step)*f/512;
                for(int phase=0;phase<=256;phase+=16){
                    std::complex<double> response{};
                    for(int j=0;j<k.taps;++j)response+=double(k.coefficients[phase*k.taps+j])*std::polar(1.,-2*pi*frequency*j);
                    maximum=std::max(maximum,std::abs(response));
                }
            }
            std::printf("worst alias-band response: %.2f dB\n",20*std::log10(maximum));
            assert(maximum<.0001);
        }
        for(int i=0;i<128;++i){float a,b,r;
            auto tone=[&](uint64_t n,int){return float(std::sin(n*2*pi*.4));};
            k.sample(2000+i+1-1e-7,tone,a,r);k.sample(2000+i+1,tone,b,r);worst=std::max(worst,double(std::abs(a-b)));}
        assert(worst<1e-5);
        std::printf("taps=%d DC group delay=%.3f samples (%.3f ms), phase wrap error %.3g\n",k.taps,lowDelay,lowDelay/rates.first*1000,worst);
    }
}
