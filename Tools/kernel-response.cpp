// Offline response sweep of the production phase table. No audio devices.
#include "InterpolationKernel.hpp"
#include <cstdio>
#include <complex>
int main() {
    constexpr double pi=3.141592653589793;
    double worst=0;int worstFrequency=0;double worstSource=0,worstOutput=0;
    for(double source:{44100.,48000.,88200.,96000.}) for(double output:{44100.,48000.,88200.,96000.}) {
        lcshared::InterpolationKernel kernel(source/output);
        for(int frequency=100;frequency<=20000;frequency+=100) {
            std::array<std::complex<double>,lcshared::InterpolationKernel::Taps> waves;
            for(int tap=0;tap<kernel.taps;++tap) waves[tap]=std::polar(1.0,2*pi*frequency*(tap-kernel.before)/source);
            for(int phase=0;phase<=lcshared::InterpolationKernel::Phases;phase+=16) {
                std::complex<double> response{};
                for(int tap=0;tap<kernel.taps;++tap) response+=double(kernel.table[phase][tap])*waves[tap];
                const double db=20*std::log10(std::abs(response));
                if(std::abs(db)>worst){worst=std::abs(db);worstFrequency=frequency;worstSource=source;worstOutput=output;}
            }
        }
    }
    std::printf("worst passband deviation=%.6f dB frequency=%d source=%.0f output=%.0f\n",worst,worstFrequency,worstSource,worstOutput);
    return worst<=0.1?0:1;
}
