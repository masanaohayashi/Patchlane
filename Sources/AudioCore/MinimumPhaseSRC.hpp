#pragma once
#include <Accelerate/Accelerate.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace patchsrc {
// Control-thread construction only. One oversampled prototype is spectrally
// factored before phase decomposition; independently factoring phase rows would
// destroy their fractional-time relationship.
struct MinimumPhaseKernel {
    static constexpr int Phases=256;
    int taps=0;
    std::vector<float> coefficients;
    explicit MinimumPhaseKernel(double step) {
        if(!std::isfinite(step)||step<=0||step>24) throw std::invalid_argument("SRC ratio");
        const double scale=std::max(1.0,step*1.003); // clock tracking headroom
        const int half=int(std::ceil(48*scale));
        taps=half*3+1;
        const int oversample=Phases;
        int log2=1;while((1<<log2)<taps*oversample*4)++log2;
        const int n=1<<log2;
        auto fft=vDSP_create_fftsetupD(log2,kFFTRadix2);
        if(!fft)throw std::bad_alloc();
        // Destroy the FFT setup even if a subsequent allocation fails.
        struct Guard { FFTSetupD p; ~Guard(){vDSP_destroy_fftsetupD(p);} } guard{fft};
        std::vector<double> re(n,0),im(n,0);
        DSPDoubleSplitComplex z{re.data(),im.data()};
        constexpr double pi=3.14159265358979323846;
        const double cutoff=.90/scale;
        for(int j=0;j<=2*half*oversample;++j) {
            const double x=double(j)/oversample-half,u=x/half;
            const double w=.35875+.48829*std::cos(pi*u)+.14128*std::cos(2*pi*u)+.01168*std::cos(3*pi*u);
            re[j]=(std::abs(x)<1e-12?cutoff:std::sin(pi*cutoff*x)/(pi*x))*w/oversample;
        }
        vDSP_fft_zipD(fft,&z,1,log2,FFT_FORWARD);
        for(int j=0;j<n;++j){re[j]=std::log(std::max(std::hypot(re[j],im[j]),1e-12));im[j]=0;}
        vDSP_fft_zipD(fft,&z,1,log2,FFT_INVERSE);
        for(int j=0;j<n;++j){re[j]*=(j==0||j==n/2?1.0:j<n/2?2.0:0.0)/n;im[j]=0;}
        vDSP_fft_zipD(fft,&z,1,log2,FFT_FORWARD);
        for(int j=0;j<n;++j){const double a=std::exp(re[j]),p=im[j];re[j]=a*std::cos(p);im[j]=a*std::sin(p);}
        vDSP_fft_zipD(fft,&z,1,log2,FFT_INVERSE);
        coefficients.resize((Phases+1)*taps);
        for(int phase=0;phase<=Phases;++phase){
            double sum=0;
            for(int k=0;k<taps;++k){const double v=re[k*oversample+phase]*oversample/n;coefficients[phase*taps+k]=float(v);sum+=v;}
            for(int k=0;k<taps;++k)coefficients[phase*taps+k]/=float(sum);
        }
    }
    template<class Read> void sample(double position,Read read,float &left,float &right) const noexcept {
        const auto whole=static_cast<uint64_t>(position);
        const double phase=(position-double(whole))*Phases;
        const int row=std::min(int(phase),Phases-1);
        const float blend=float(phase-row);
        const float *a=coefficients.data()+row*taps,*b=a+taps;
        left=right=0;
        for(int k=0;k<taps;++k){
            if(whole<static_cast<uint64_t>(k))break; // zero history at stream start
            const float weight=a[k]+(b[k]-a[k])*blend;
            left+=read(whole-k,0)*weight;right+=read(whole-k,1)*weight;
        }
    }
};
}
