#include "../../Sources/AudioCore/DipoleDSP.hpp"
#include <cstdio>
#include <cassert>
#include <chrono>
#include <thread>
using namespace patchdipole;
LCDipoleConfig defaults(){LCDipoleConfig c{};c.spacingCM=20;c.distanceCM=100;c.headCM=17.5;c.maxBoostDB=12;c.tonalReference=1;return c;}
Z at(const std::vector<float>&h,double f,double rate){Z result=0;for(size_t i=0;i<h.size();++i)result+=double(h[i])*std::polar(1.,-2*Pi*f*i/rate);return result;}
// Deliberately unoptimized four-path/full-spectrum reference, same coefficients.
struct Reference {
    FFTSetup fft=vDSP_create_fftsetup(9,kFFTRadix2);
    std::array<std::array<Spectrum,32>,2> history{};
    std::array<std::array<Spectrum,32>,2> kernels{};
    std::array<std::array<float,256>,2> pending{},output{},overlap{};
    Spectrum sum;
    float gain;int index=0,head=0;
    explicit Reference(const Coefficients &c):gain(c.gain) {
        for(int p=0;p<32;++p)for(int k=0;k<512;++k) {
            kernels[0][p].re[k]=(c.spectra[0][p].re[k]+c.spectra[1][p].re[k])*.5f;
            kernels[0][p].im[k]=(c.spectra[0][p].im[k]+c.spectra[1][p].im[k])*.5f;
            kernels[1][p].re[k]=(c.spectra[0][p].re[k]-c.spectra[1][p].re[k])*.5f;
            kernels[1][p].im[k]=(c.spectra[0][p].im[k]-c.spectra[1][p].im[k])*.5f;
        }
    }
    ~Reference(){vDSP_destroy_fftsetup(fft);}
    void process(float *s,int frames){for(int f=0;f<frames;++f){
        pending[0][index]=s[2*f];pending[1][index]=s[2*f+1];
        s[2*f]=output[0][index]*gain;s[2*f+1]=output[1][index]*gain;
        if(++index==256){index=0;
            for(int ch=0;ch<2;++ch){auto &x=history[ch][head];x.re.fill(0);x.im.fill(0);std::copy(pending[ch].begin(),pending[ch].end(),x.re.begin());auto split=x.split();vDSP_fft_zip(fft,&split,1,9,FFT_FORWARD);}
            for(int out=0;out<2;++out){sum.re.fill(0);sum.im.fill(0);
                for(int p=0;p<32;++p)for(int in=0;in<2;++in){auto &x=history[in][(head-p+32)%32],&h=kernels[in==out?0:1][p];
                    for(int k=0;k<512;++k){sum.re[k]+=x.re[k]*h.re[k]-x.im[k]*h.im[k];sum.im[k]+=x.re[k]*h.im[k]+x.im[k]*h.re[k];}}
                auto split=sum.split();vDSP_fft_zip(fft,&split,1,9,FFT_INVERSE);
                for(int i=0;i<256;++i){output[out][i]=sum.re[i]/512+overlap[out][i];overlap[out][i]=sum.re[i+256]/512;}}
            head=(head+1)%32;
        }
    }}
};
int main(){
    // Independent, analytic minimum-phase fixture: [0.5,1] has an outside
    // zero and must reconstruct to [1,0.5], not merely be shifted earlier.
    constexpr int synthesis=Coefficients::SynthesisSize;
    FFTSetup cepstralFFT=vDSP_create_fftsetup(Coefficients::SynthesisLog2,kFFTRadix2);
    assert(cepstralFFT);
    std::vector<Z> mixed(synthesis/2+1);
    for(int k=0;k<=synthesis/2;++k)mixed[k]=.5+std::polar(1.,-2*Pi*k/synthesis);
    const auto minimum=minimumPhaseSpectrum(mixed,cepstralFFT,Coefficients::SynthesisLog2);
    double spectralError=0;
    for(int k=0;k<=synthesis/2;++k)spectralError=std::max(spectralError,std::abs(minimum[k]-(1.+std::polar(.5,-2*Pi*k/synthesis))));
    assert(spectralError<2e-5);vDSP_destroy_fftsetup(cepstralFFT);
    std::printf("Analytic cepstral minimum-phase fixture: max complex error %.8g\n",spectralError);
    auto config=defaults();auto coefficients=std::make_shared<Coefficients>(config,44100);
    // Exact optimized-vs-full-matrix comparison through real streaming state.
    Processor fast(coefficients);fast.dormant=false;fast.blend=1;
    auto reference=std::make_unique<Reference>(*coefficients);
    std::array<float,2048> a{},b{};
    for(int block=0;block<60;++block){int n=std::array<int,5>{1,17,32,255,1024}[block%5];for(int i=0;i<n*2;++i)a[i]=b[i]=float(.01*std::sin((block*2048+i)*.37));
        fast.process(a.data(),2,a.data()+1,2,n,true);reference->process(b.data(),n);
        for(int i=0;i<n*2;++i)assert(std::abs(a[i]-b[i])<2e-6);
    }
    // Compare FFT convolution against independently evaluated time-domain impulse.
    Processor impulse(coefficients);impulse.dormant=false;impulse.blend=1;
    for(int i=0;i<9000;++i){float l=i==0?.1f:0,r=0;impulse.process(&l,1,&r,1,1,true);
        if(i>=256&&i<8448){int k=i-256;assert(std::abs(l-.05f*(coefficients->impulse[0][k]+coefficients->impulse[1][k])*coefficients->gain)<1e-6);assert(std::abs(r-.05f*(coefficients->impulse[0][k]-coefficients->impulse[1][k])*coefficients->gain)<1e-6);}}
    for(double rate:{44100.,48000.,88200.,96000.}) {
        Coefficients c(config,rate);
        for(double f:{20.,50.,100.}) {double mid=20*std::log10(std::abs(at(c.impulse[0],f,rate))),side=20*std::log10(std::abs(at(c.impulse[1],f,rate)));assert(std::abs(mid)<.3&&std::abs(side)<.5);}
    }
    // Check independent target magnitudes and front-loaded energy across the
    // supported rates and geometry extremes. Phase equality is intentionally
    // not asserted: standard minimum phase discards the old modal phase.
    double magnitudeError=0;
    for(double rate:{44100.,48000.,88200.,96000.})for(int geometry=0;geometry<3;++geometry)for(int tone=0;tone<3;++tone) {
        auto c=config;c.tonalReference=tone;
        if(geometry==1){c.spacingCM=2;c.distanceCM=500;c.headCM=10;c.maxBoostDB=24;}
        if(geometry==2){c.spacingCM=40;c.distanceCM=20;c.headCM=25;}
        Coefficients h(c,rate);assert(h.gain==1);
        for(int mode=0;mode<2;++mode) {
            const auto peak=std::max_element(h.impulse[mode].begin(),h.impulse[mode].end(),[](float a,float b){return std::abs(a)<std::abs(b);});
            assert(std::distance(h.impulse[mode].begin(),peak)<64);
            for(double f:{20.,50.,100.,200.,300.,500.,1000.,3000.,6000.,8000.,10000.,16000.,20000.}) {
                const double error=std::abs(20*std::log10(std::abs(at(h.impulse[mode],f,rate))/std::abs(response(c,f,rate,false)[mode])));
                magnitudeError=std::max(magnitudeError,error);assert(error<.15);
            }
        }
    }
    std::printf("Modal target magnitudes: worst error %.6f dB; impulse peaks <64 samples\n",magnitudeError);
    for(int band:{0,17,30})for(double db:{-12.,12.}) {
        auto c=config;c.geqDB[band]=db;auto h=std::make_shared<Coefficients>(c,44100);Processor p(h);p.dormant=false;p.blend=1;
        constexpr double frequencies[3]={20,1000,20000};double f=frequencies[band==0?0:band==17?1:2],energy=0,inputEnergy=0;
        for(int n=0;n<88200;++n){float x=float(.001*std::sin(2*Pi*f*n/44100)),l=x,r=x;p.process(&l,1,&r,1,1,true);if(n>=44100){energy+=double(l)*l;inputEnergy+=double(x)*x;}}
        double result=10*std::log10(energy/inputEnergy)-20*std::log10(h->gain);
        std::printf("GEQ %gHz: target %+.1f, actual %+.3f dB\n",f,db,result);assert(std::abs(result-db)<.15);
    }
    auto seconds=[](auto operation){auto t=std::chrono::steady_clock::now();operation();return std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();};
    Processor bench(coefficients);bench.dormant=false;bench.blend=1;auto old=std::make_unique<Reference>(*coefficients);
    constexpr int total=44100*10;
    double modern=seconds([&]{for(int n=0;n<total;n+=32){a.fill(.001f);bench.process(a.data(),2,a.data()+1,2,32,true);}});
    double original=seconds([&]{for(int n=0;n<total;n+=32){a.fill(.001f);old->process(a.data(),32);}});
    Effect effect;effect.configure(config,44100);
    double bypass=seconds([&]{for(int n=0;n<total;n+=32){a.fill(.001f);effect.process(a.data(),2,a.data()+1,2,32);}});
    std::printf("10s, 32-frame callbacks: optimized %.6fs; 4-path reference %.6fs; bypass %.6fs; speedup %.2fx\n",modern,original,bypass,original/modern);
    auto eqConfig=config;for(int i=0;i<31;++i)eqConfig.geqDB[i]=i%2?3:-3;
    Processor fullEQ(std::make_shared<Coefficients>(eqConfig,44100));fullEQ.dormant=false;fullEQ.blend=1;
    double worst=seconds([&]{for(int n=0;n<total;n+=32){a.fill(.001f);fullEQ.process(a.data(),2,a.data()+1,2,32,true);}});
    std::printf("10s with all 31 GEQ bands active: %.6fs (one-output DSP %.3f%% of one core)\n",worst,worst/10*100);
    assert(modern<1); // broad real-time budget, not an unstable microbenchmark speed-ratio gate
    // Concurrent control-side allocation/publication/reclamation and callback processing.
    std::atomic<bool> done{false};std::thread audio([&]{std::array<float,64>s{};while(!done.load()){s.fill(.001f);effect.process(s.data(),2,s.data()+1,2,32);for(float x:s)assert(std::isfinite(x));}});
    for(int i=0;i<40;++i){config.spacingCM=10+i;effect.configure(config,44100);effect.enabled.store(i%2==0);}
    done=true;audio.join();
    std::puts("PASS: cepstral minimum phase, modal magnitudes/early impulse, FFT/time-domain impulse, full-matrix/NEON parity, sample rates, bounded CPU and live publication");
}
