#pragma once
#include "include/AudioCore.h"
#include <Accelerate/Accelerate.h>
#include <array>
#include <vector>
#include <complex>
#include <atomic>
#include <memory>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
#include <arm_neon.h>
#endif
namespace patchdipole {
constexpr double Pi=3.14159265358979323846;
using Z=std::complex<double>;
inline Z shadow(double theta,double radius,double f) {
    double alpha=1.05+0.95*std::cos(theta/(5*Pi/6)*Pi),wt=2*Pi*f*2*radius/343;
    double dt=theta<=Pi/2 ? -radius/343*std::cos(theta):radius/343*(theta-Pi/2);
    return Z(1,alpha*wt)/Z(1,wt)*std::polar(1.,-2*Pi*f*dt);
}
inline bool valid(const LCDipoleConfig &c) {
    if(!std::isfinite(c.spacingCM)||!std::isfinite(c.distanceCM)||!std::isfinite(c.headCM)||!std::isfinite(c.maxBoostDB)||!std::isfinite(c.trimDB))return false;
    if(c.spacingCM<2||c.spacingCM>200||c.distanceCM<20||c.distanceCM>500||c.headCM<10||c.headCM>25||c.maxBoostDB<0||c.maxBoostDB>24||c.trimDB<-12||c.trimDB>12||c.tonalReference<0||c.tonalReference>2||std::atan2(c.spacingCM/2,c.distanceCM)>Pi/4)return false;
    for(double db:c.geqDB)if(!std::isfinite(db)||db<-12||db>12)return false;
    return true;
}
inline std::array<Z,2> response(const LCDipoleConfig &c,double f,double rate,bool includeGEQ=true) {
    double radius=c.headCM/200,x=c.spacingCM/200,z=c.distanceCM/100,angle=std::atan2(x,z),r=std::hypot(x,z);
    auto front=shadow(Pi/2,radius,f);
    auto a=shadow(Pi/2-angle,radius,f)/front*(r/std::hypot(x-radius,z));
    auto b=shadow(Pi/2+angle,radius,f)/front*(r/std::hypot(x+radius,z));
    double maxGain=std::pow(10,c.maxBoostDB/20),lambda=1/(4*maxGain*maxGain);
    auto inv=[&](Z h){return std::conj(h)/(std::norm(h)+lambda);};
    Z mid=inv(a+b),side=inv(a-b);
    if(c.tonalReference==1) { double scale=1/std::max(std::abs(mid),1e-9);mid*=scale;side*=scale; }
    if(c.tonalReference==2) { double scale=1/std::max(std::sqrt((std::norm(mid)+std::norm(side))/2),1e-9);mid*=scale;side*=scale; }
    double peak=std::max(std::abs(mid),std::abs(side));
    if(peak>maxGain){mid*=maxGain/peak;side*=maxGain/peak;}
    if(f<300) {
        double keep=f<=100?1:0.5+0.5*std::cos(Pi*(f-100)/200),mp=std::arg(mid),sp=std::abs(side)>1e-9?std::arg(side):mp;
        double delta=std::atan2(std::sin(sp-mp),std::cos(sp-mp)),phase=f>=100?1:0.5-0.5*std::cos(Pi*f/100);
        mid=std::polar(std::pow(std::max(std::abs(mid),1e-9),1-keep),mp);
        side=std::polar(std::pow(std::max(std::abs(side),1e-9),1-keep),sp*(1-keep)+(mp+delta*phase)*keep);
    }
    double weight=f<=6000?1:f>=10000?0:0.5+0.5*std::cos(Pi*(f-6000)/4000);
    mid=mid*weight+Z(1-weight,0);side=side*weight+Z(1-weight,0);
    double eqWeight=c.tonalReference==1||f<=200?1:f>=500?0:0.5+0.5*std::cos(Pi*(f-200)/300);
    double eq=std::pow(std::min(std::pow(10,24./20),1/std::max(std::abs(mid),1e-9)),eqWeight);
    Z geq=includeGEQ?std::pow(10,c.trimDB/20):1;
    constexpr double centres[31]={20,25,31.5,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000};
    Z v=std::polar(1.,-2*Pi*f/rate);
    for(int i=0;i<31;++i)if(includeGEQ&&c.geqDB[i]!=0) {
        double A=std::pow(10,c.geqDB[i]/40),w=2*Pi*centres[i]/rate,alpha=std::sin(w)/(2*4.318),co=std::cos(w);
        geq*=((1+alpha*A)-2*co*v+(1-alpha*A)*v*v)/((1+alpha/A)-2*co*v+(1-alpha/A)*v*v);
    }
    return {mid*eq*geq,side*eq*geq};
}
// Standard homomorphic minimum-phase reconstruction. This deliberately keeps
// only log magnitude, not the old phase: the two modal phases can change.
// All synthesis allocations and FFT setup work run on the control thread.
inline std::vector<Z> minimumPhaseSpectrum(const std::vector<Z> &positive,FFTSetup fft,int log2) {
    const int n=1<<log2;
    if(positive.size()!=size_t(n/2+1))throw std::invalid_argument("Invalid synthesis spectrum");
    std::vector<float> real(n),imag(n,0);
    for(int k=0;k<=n/2;++k) {
        real[k]=float(std::log(std::max(std::abs(positive[k]),1e-12)));
        if(k>0&&k<n/2)real[n-k]=real[k];
    }
    DSPSplitComplex split{real.data(),imag.data()};
    vDSP_fft_zip(fft,&split,1,log2,FFT_INVERSE);
    // The real cepstrum is even. Fold its negative-time half onto positive
    // time to obtain a causal complex cepstrum, retaining DC and Nyquist.
    real[0]/=n;real[n/2]/=n;
    for(int i=1;i<n/2;++i)real[i]*=2.f/n;
    std::fill(real.begin()+n/2+1,real.end(),0.f);std::fill(imag.begin(),imag.end(),0.f);
    vDSP_fft_zip(fft,&split,1,log2,FFT_FORWARD);
    std::vector<Z> result(n/2+1);
    for(int k=0;k<=n/2;++k)result[k]=std::exp(Z(real[k],imag[k]));
    // Real FIR endpoints; magnitude reconstruction has positive DC/Nyquist.
    result[0]=Z(result[0].real(),0);result[n/2]=Z(result[n/2].real(),0);
    return result;
}
// Immutable coefficients can be shared; histories belong to each output callback.
struct Coefficients {
    static constexpr int MaxTaps=1024;
    static int tapsForRate(double rate) {
        int taps=256;
        while(rate>48000.*(taps/256)&&taps<MaxTaps)taps*=2;
        return taps;
    }
    const int taps;
    static constexpr int SynthesisLog2=15,SynthesisSize=1<<SynthesisLog2;
    double rate;
    LCDipoleConfig settings;
    alignas(64) std::array<std::array<float,MaxTaps>,2> reversed{};
    std::array<int,2> activeTaps{};
    std::array<std::vector<float>,2> impulse;
    float gain=1;
    explicit Coefficients(const LCDipoleConfig &c,double r):taps(tapsForRate(r)),rate(r),settings(c) {
        if(!valid(c)||(r!=44100&&r!=48000&&r!=88200&&r!=96000))throw std::invalid_argument("Invalid dipole settings");
        FFTSetup large=vDSP_create_fftsetup(SynthesisLog2,kFFTRadix2);
        if(!large)throw std::bad_alloc();
        struct Cleanup { FFTSetup fft;~Cleanup(){vDSP_destroy_fftsetup(fft);} }cleanup{large};
        std::array<std::vector<Z>,2> target;
        for(auto &v:target)v.resize(SynthesisSize/2+1);
        for(int k=0;k<=SynthesisSize/2;++k) {
            auto h=response(c,double(k)*r/SynthesisSize,r,false);
            for(int mode=0;mode<2;++mode)target[mode][k]=h[mode];
        }
        // Never attenuate automatically. Output clipping is handled by the mixer.
        for(int mode=0;mode<2;++mode) {
            const auto minimum=minimumPhaseSpectrum(target[mode],large,SynthesisLog2);
            std::vector<float> real(SynthesisSize),imag(SynthesisSize);
            for(int k=0;k<=SynthesisSize/2;++k) {
                real[k]=float(minimum[k].real());imag[k]=float(minimum[k].imag());
                if(k>0&&k<SynthesisSize/2){real[SynthesisSize-k]=real[k];imag[SynthesisSize-k]=-imag[k];}
            }
            DSPSplitComplex time{real.data(),imag.data()};vDSP_fft_zip(large,&time,1,SynthesisLog2,FFT_INVERSE);
            impulse[mode].resize(taps);
            // Keep exactly the requested duration, gently tapering the last 1/8
            // to zero. No leading fade, design delay, normalization or attenuation.
            const int tail=taps/8;
            for(int i=0;i<taps;++i) {
                const float window=i<taps-tail?1.f:float(.5+.5*std::cos(Pi*(i-(taps-tail))/(tail-1)));
                const float value=real[i]/SynthesisSize*window;
                // Remove FFT roundoff dust only; a flat Mid path becomes one MAC.
                impulse[mode][i]=std::abs(value)<1e-12f?0.f:value;
            }
            int active=taps;
            while(active>1&&impulse[mode][active-1]==0.f)--active;
            activeTaps[mode]=active;
            std::reverse_copy(impulse[mode].begin(),impulse[mode].begin()+active,reversed[mode].begin());
        }
    }
};
inline float dot(const float *samples,const float *kernel,int count) {
    float result=0;int k=0;
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
    auto a=vdupq_n_f32(0),b=a,c=a,d=a;
    for(;k+16<=count;k+=16) {
        a=vfmaq_f32(a,vld1q_f32(samples+k),vld1q_f32(kernel+k));
        b=vfmaq_f32(b,vld1q_f32(samples+k+4),vld1q_f32(kernel+k+4));
        c=vfmaq_f32(c,vld1q_f32(samples+k+8),vld1q_f32(kernel+k+8));
        d=vfmaq_f32(d,vld1q_f32(samples+k+12),vld1q_f32(kernel+k+12));
    }
    a=vaddq_f32(vaddq_f32(a,b),vaddq_f32(c,d));
    for(;k+4<=count;k+=4)a=vfmaq_f32(a,vld1q_f32(samples+k),vld1q_f32(kernel+k));
    result=vaddvq_f32(a);
#endif
    for(;k<count;++k)result+=samples[k]*kernel[k];
    return result;
}
struct Biquad {
    double b0,b1,b2,a1,a2;
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
    float64x2_t z1=vdupq_n_f64(0),z2=vdupq_n_f64(0);
#else
    std::array<double,2> z1{},z2{};
#endif
    Biquad(double f,double db,double rate) {
        double A=std::pow(10,db/40),w=2*Pi*f/rate,alpha=std::sin(w)/(2*4.318),co=std::cos(w),den=1+alpha/A;
        b0=(1+alpha*A)/den;b1=-2*co/den;b2=(1-alpha*A)/den;a1=b1;a2=(1-alpha/A)/den;
    }
    void reset() {
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
        z1=z2=vdupq_n_f64(0);
#else
        z1.fill(0);z2.fill(0);
#endif
    }
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
    float64x2_t process(float64x2_t x) {
        auto y=vfmaq_f64(z1,x,vdupq_n_f64(b0));
        z1=vaddq_f64(vfmsq_f64(vmulq_n_f64(x,b1),y,vdupq_n_f64(a1)),z2);
        z2=vfmsq_f64(vmulq_n_f64(x,b2),y,vdupq_n_f64(a2));
        return y;
    }
#else
    void process(double &l,double &r) {
        double y[2]={b0*l+z1[0],b0*r+z1[1]},x[2]={l,r};
        for(int i=0;i<2;++i){z1[i]=b1*x[i]-a1*y[i]+z2[i];z2[i]=b2*x[i]-a2*y[i];}
        l=y[0];r=y[1];
    }
#endif
};
struct Processor {
    std::shared_ptr<const Coefficients> coefficients;
    std::vector<Biquad> eq;
    float trim=1;
    // Mirrored circular histories give contiguous oldest-to-newest samples.
    alignas(64) std::array<std::array<float,Coefficients::MaxTaps*2>,2> history{};
    int index=0;
    bool dormant=true;
    float blend=0;
    explicit Processor(std::shared_ptr<const Coefficients> c):coefficients(std::move(c)) {
        constexpr double centres[31]={20,25,31.5,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000};
        eq.reserve(31);
        for(int i=0;i<31;++i)if(coefficients->settings.geqDB[i]!=0)eq.emplace_back(centres[i],coefficients->settings.geqDB[i],coefficients->rate);
        trim=float(std::pow(10,coefficients->settings.trimDB/20));
    }
    void process(float *left,int leftStride,float *right,int rightStride,int frames,bool enabled) {
        if(!enabled&&blend==0){dormant=true;return;}
        if(enabled&&dormant) {
            index=0;
            for(auto &v:history)v.fill(0);
            for(auto &band:eq)band.reset();
            dormant=false;
        }
        const float step=float(1/(coefficients->rate*.01));
        for(int f=0;f<frames;++f) {
            float l=left[f*leftStride],r=right[f*rightStride];
            float el=l,er=r;
            if(!eq.empty()) {
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
                double samples[2]={l,r};auto pair=vld1q_f64(samples);
                for(auto &band:eq)pair=band.process(pair);
                vst1q_f64(samples,pair);el=float(samples[0]);er=float(samples[1]);
#else
                double dl=l,dr=r;for(auto &band:eq)band.process(dl,dr);el=float(dl);er=float(dr);
#endif
            }
            el*=trim;er*=trim;
            // Linked-stereo GEQ commutes with the scalar correction; the modal
            // FIR combines correction and dipole with no block buffering.
            const float modal[2]={(el+er)*.5f,(el-er)*.5f};
            float filtered[2];
            for(int mode=0;mode<2;++mode) {
                auto &past=history[mode];past[index]=past[index+Coefficients::MaxTaps]=modal[mode];
                const int count=coefficients->activeTaps[mode];
                filtered[mode]=dot(past.data()+index+Coefficients::MaxTaps+1-count,coefficients->reversed[mode].data(),count);
            }
            const float wl=(filtered[0]+filtered[1])*coefficients->gain,wr=(filtered[0]-filtered[1])*coefficients->gain;
            blend=enabled?std::min(1.f,blend+step):std::max(0.f,blend-step);
            left[f*leftStride]=l+(wl-l)*blend;right[f*rightStride]=r+(wr-r)*blend;
            index=(index+1)&(Coefficients::MaxTaps-1);
        }
        if(!enabled&&blend==0)dormant=true;
    }
};
// Control thread owns allocation and reclamation. Audio only reads atomics and computes.
class Effect {
    std::atomic<Processor*> active{nullptr};
    std::atomic<int> readers{0};
    std::atomic<bool> bypassed{true};
    std::vector<std::unique_ptr<Processor>> retired;
    std::unique_ptr<Processor> owned;
public:
    std::atomic<bool> enabled{false};
    void configure(const LCDipoleConfig &c,double rate) {
        auto replacement=std::make_unique<Processor>(std::make_shared<Coefficients>(c,rate));
        // Reserve retirement ownership before publishing; allocation failure cannot
        // leave the callback pointing at a destroyed replacement.
        retired.push_back(nullptr);
        active.exchange(replacement.get());
        retired.back()=std::move(owned);owned=std::move(replacement);
        if(readers.load()==0)retired.clear();
    }
    void process(float *l,int ls,float *r,int rs,int n) {
        const bool on=enabled.load(std::memory_order_relaxed);
        if(!on&&bypassed.load(std::memory_order_relaxed))return;
        readers.fetch_add(1);
        auto *p=active.load();
        if(p){p->process(l,ls,r,rs,n,on);bypassed.store(!on&&p->blend==0,std::memory_order_relaxed);}
        readers.fetch_sub(1);

    }
};
}
