#pragma once
#include <array>
#include <cmath>
#include <algorithm>
namespace lcshared {
// Prepared on the control thread. Blackman-windowed sinc, interpolated
// phase table. Same-rate reading needs floor(t)-23 .. floor(t)+24;
// downsampling scales the support by the source/output rate ratio.
// This lookahead is not a hidden queue: unavailable frames are rejected.
struct InterpolationKernel {
    static constexpr int Taps=192,Phases=512;
    std::array<std::array<float,Taps>,Phases+1> table{};
    double nominalStep=1;
    int before=23,after=24,taps=48;
    explicit InterpolationKernel(double step=1) { prepare(step); }
    void prepare(double step) {
        if(!std::isfinite(step)||step<=0||step>3) {nominalStep=0;return;}
        nominalStep=step;
        after=std::min(96,int(std::ceil(24*std::max(1.0,step))));before=after-1;taps=2*after;
        constexpr double pi=3.141592653589793;
        const double cutoff=step>1.01?0.99/step:1;
        for(int phase=0;phase<=Phases;++phase) {
            const double fraction=double(phase)/Phases;
            double sum=0;
            for(int tap=0;tap<taps;++tap) {
                const double x=tap-before-fraction;
                const double window=0.42+0.5*std::cos(pi*x/after)+0.08*std::cos(2*pi*x/after);
                const double value=(std::abs(x)<1e-12?cutoff:std::sin(pi*cutoff*x)/(pi*x))*window;
                table[phase][tap]=float(value);sum+=value;
            }
            for(auto &value:table[phase]) value=float(value/sum);
        }
    }
    bool supports(double step) const {return nominalStep>0&&std::isfinite(step)&&std::abs(step/nominalStep-1)<0.005;}
};
}
