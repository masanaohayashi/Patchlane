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
struct Spectrum {
    alignas(64) std::array<float,512> re{},im{};
    DSPSplitComplex split() { return {re.data(),im.data()}; }
};
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
    static constexpr int Taps=8192,Block=256,FFT=512,Parts=Taps/Block;
    static constexpr int SynthesisLog2=15,SynthesisSize=1<<SynthesisLog2;
    double rate;
    LCDipoleConfig settings;
    std::array<std::array<Spectrum,Parts>,2> spectra{};
    std::array<std::vector<float>,2> impulse;
    float gain=1;
    explicit Coefficients(const LCDipoleConfig &c,double r):rate(r),settings(c) {
        if(!valid(c)||(r!=44100&&r!=48000&&r!=88200&&r!=96000))throw std::invalid_argument("Invalid dipole settings");
        FFTSetup large=vDSP_create_fftsetup(SynthesisLog2,kFFTRadix2),small=vDSP_create_fftsetup(9,kFFTRadix2);
        if(!large||!small){if(large)vDSP_destroy_fftsetup(large);if(small)vDSP_destroy_fftsetup(small);throw std::bad_alloc();}
        struct Cleanup { FFTSetup a,b;~Cleanup(){vDSP_destroy_fftsetup(a);vDSP_destroy_fftsetup(b);} }cleanup{large,small};
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
            impulse[mode].resize(Taps);
            // One-sided tail taper: retain the leading impulse at sample zero.
            // The longer synthesis grid keeps cepstral circular aliasing small.
            constexpr int Tail=Taps/8;
            for(int i=0;i<Taps;++i) {
                const float window=i<Taps-Tail?1.f:float(.5+.5*std::cos(Pi*(i-(Taps-Tail))/(Tail-1)));
                impulse[mode][i]=real[i]/SynthesisSize*window;
            }
            for(int p=0;p<Parts;++p) {
                auto &s=spectra[mode][p];std::copy_n(impulse[mode].data()+p*Block,Block,s.re.begin());
                auto split=s.split();vDSP_fft_zip(small,&split,1,9,FFT_FORWARD);
            }
        }
    }
};
inline void accumulate(Spectrum &sum,const Spectrum &x,const Spectrum &h) {
    // Only positive frequencies: real FIR/input are Hermitian. Four bins per SIMD lane group.
    int k=0;
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
    for(;k<256;k+=4) {
        auto xr=vld1q_f32(x.re.data()+k),xi=vld1q_f32(x.im.data()+k),hr=vld1q_f32(h.re.data()+k),hi=vld1q_f32(h.im.data()+k);
        auto re=vld1q_f32(sum.re.data()+k),im=vld1q_f32(sum.im.data()+k);
        re=vfmsq_f32(vfmaq_f32(re,xr,hr),xi,hi);im=vfmaq_f32(vfmaq_f32(im,xr,hi),xi,hr);
        vst1q_f32(sum.re.data()+k,re);vst1q_f32(sum.im.data()+k,im);
    }
#endif
    for(;k<=256;++k){sum.re[k]+=x.re[k]*h.re[k]-x.im[k]*h.im[k];sum.im[k]+=x.re[k]*h.im[k]+x.im[k]*h.re[k];}
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
    FFTSetup fft=nullptr;
    std::vector<Biquad> eq;
    float trim=1;
    std::array<std::array<Spectrum,Coefficients::Parts>,2> history{};
    std::array<std::array<float,256>,2> pending{},output{},overlap{};
    Spectrum sum;
    int index=0,head=0,blocksSeen=0,startup=0;
    bool dormant=true;
    float blend=0;
    explicit Processor(std::shared_ptr<const Coefficients> c):coefficients(std::move(c)) {
        constexpr double centres[31]={20,25,31.5,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000};
        eq.reserve(31);
        for(int i=0;i<31;++i)if(coefficients->settings.geqDB[i]!=0)eq.emplace_back(centres[i],coefficients->settings.geqDB[i],coefficients->rate);
        trim=float(std::pow(10,coefficients->settings.trimDB/20));
        fft=vDSP_create_fftsetup(9,kFFTRadix2);if(!fft)throw std::bad_alloc();
    }
    ~Processor(){vDSP_destroy_fftsetup(fft);}
    void block() {
        for(int mode=0;mode<2;++mode) {
            auto &x=history[mode][head];x.re.fill(0);x.im.fill(0);std::copy(pending[mode].begin(),pending[mode].end(),x.re.begin());
            auto split=x.split();vDSP_fft_zip(fft,&split,1,9,FFT_FORWARD);
            sum.re.fill(0);sum.im.fill(0);
            for(int p=0;p<std::min(blocksSeen+1,Coefficients::Parts);++p)accumulate(sum,history[mode][(head-p+Coefficients::Parts)%Coefficients::Parts],coefficients->spectra[mode][p]);
            sum.im[0]=sum.im[256]=0;
            for(int k=1;k<256;++k){sum.re[512-k]=sum.re[k];sum.im[512-k]=-sum.im[k];}
            split=sum.split();vDSP_fft_zip(fft,&split,1,9,FFT_INVERSE);
            for(int i=0;i<256;++i){output[mode][i]=sum.re[i]/512+overlap[mode][i];overlap[mode][i]=sum.re[i+256]/512;}
        }
        head=(head+1)%Coefficients::Parts;blocksSeen=std::min(Coefficients::Parts,blocksSeen+1);
    }
    void process(float *left,int leftStride,float *right,int rightStride,int frames,bool enabled) {
        if(!enabled&&blend==0){dormant=true;return;}
        if(enabled&&dormant) {
            index=head=blocksSeen=0;startup=Coefficients::Block;
            for(auto &v:pending)v.fill(0);for(auto &v:output)v.fill(0);for(auto &v:overlap)v.fill(0);
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
            // Same L/R GEQ commutes with the fixed scalar correction FIR.
            // Correction+matrix share the FFT; equivalent order: correction -> GEQ -> matrix.
            pending[0][index]=(el+er)*.5f;pending[1][index]=(el-er)*.5f;
            float wl=(output[0][index]+output[1][index])*coefficients->gain,wr=(output[0][index]-output[1][index])*coefficients->gain;
            if(startup>0)--startup;
            else blend=enabled?std::min(1.f,blend+step):std::max(0.f,blend-step);
            left[f*leftStride]=l+(wl-l)*blend;right[f*rightStride]=r+(wr-r)*blend;
            if(++index==256){block();index=0;}
        }
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
