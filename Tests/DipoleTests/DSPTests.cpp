#include "../../Sources/AudioCore/DipoleDSP.hpp"
#include <cstdio>
#include <cassert>
#include <chrono>
#include <thread>
using namespace patchdipole;
LCDipoleConfig defaults(){LCDipoleConfig c{};c.spacingCM=20;c.distanceCM=100;c.headCM=17.5;c.maxBoostDB=12;c.tonalReference=1;return c;}
Z at(const std::vector<float>&h,double f,double rate){Z result=0;for(size_t i=0;i<h.size();++i)result+=double(h[i])*std::polar(1.,-2*Pi*f*i/rate);return result;}
// Independent four-path time-domain reference with separate L/R histories.
struct Reference {
    const Coefficients &c;
    std::array<std::array<float,Coefficients::MaxTaps>,2> past{};
    int position=0;
    explicit Reference(const Coefficients &coeff):c(coeff) {}
    void process(float *s,int frames){for(int f=0;f<frames;++f){
        past[0][position]=s[2*f];past[1][position]=s[2*f+1];
        double left=0,right=0;
        for(int k=0;k<c.taps;++k){
            const int j=(position-k+Coefficients::MaxTaps)%Coefficients::MaxTaps;
            const double direct=(double(c.impulse[0][k])+c.impulse[1][k])*.5;
            const double cross=(double(c.impulse[0][k])-c.impulse[1][k])*.5;
            left+=past[0][j]*direct+past[1][j]*cross;
            right+=past[1][j]*direct+past[0][j]*cross;
        }
        s[2*f]=float(left*c.gain);s[2*f+1]=float(right*c.gain);
        position=(position+1)%Coefficients::MaxTaps;
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
    // Every streamed sample agrees with the FIR at the same index: no buffering.
    for(double rate:{44100.,48000.,88200.,96000.}) {
        auto h=std::make_shared<Coefficients>(config,rate);
        assert(h->taps==(rate<=48000?256:512));
        for(const auto &mode:h->impulse){assert(mode.size()==size_t(h->taps));assert(mode.back()==0);}
        // Reconstruct the untapered Side independently and verify the exact
        // unchanged prefix and raised-cosine tail, including its zero endpoint.
        FFTSetup fft=vDSP_create_fftsetup(Coefficients::SynthesisLog2,kFFTRadix2);
        std::vector<Z> target(synthesis/2+1);
        for(int k=0;k<=synthesis/2;++k)target[k]=response(config,double(k)*rate/synthesis,rate,false)[1];
        auto spectrum=minimumPhaseSpectrum(target,fft,Coefficients::SynthesisLog2);
        std::vector<float> re(synthesis),im(synthesis);
        for(int k=0;k<=synthesis/2;++k){re[k]=float(spectrum[k].real());im[k]=float(spectrum[k].imag());
            if(k>0&&k<synthesis/2){re[synthesis-k]=re[k];im[synthesis-k]=-im[k];}}
        DSPSplitComplex split{re.data(),im.data()};vDSP_fft_zip(fft,&split,1,Coefficients::SynthesisLog2,FFT_INVERSE);
        for(int k=0;k<h->taps;++k){
            const int start=h->taps*7/8,tail=h->taps/8;
            const double fade=k<start?1:.5+.5*std::cos(Pi*(k-start)/(tail-1));
            assert(std::abs(h->impulse[1][k]-re[k]/synthesis*fade)<1e-6);
        }
        vDSP_destroy_fftsetup(fft);
        Processor parity(h);parity.dormant=false;parity.blend=1;Reference matrix(*h);
        for(int block=0;block<20;++block){
            const int n=std::array<int,5>{1,17,32,255,1024}[block%5];
            for(int i=0;i<n*2;++i)a[i]=b[i]=float(.01*std::sin((block*2048+i)*.37));
            parity.process(a.data(),2,a.data()+1,2,n,true);matrix.process(b.data(),n);
            for(int i=0;i<n*2;++i)assert(std::abs(a[i]-b[i])<2e-6);
        }
        Processor impulse(h);impulse.dormant=false;impulse.blend=1;
        for(int i=0;i<h->taps+2048;++i){float l=i==0?.1f:0,r=0;impulse.process(&l,1,&r,1,1,true);
            const float mid=i<h->taps?h->impulse[0][i]:0,side=i<h->taps?h->impulse[1][i]:0;
            assert(std::abs(l-.05f*(mid+side))<1e-6);
            assert(std::abs(r-.05f*(mid-side))<1e-6);
        }
        impulse.process(a.data(),2,a.data()+1,2,1024,false);assert(impulse.dormant);
        a.fill(0);impulse.process(a.data(),2,a.data()+1,2,1024,true);
        for(float value:a)assert(value==0);
    }
    assert(Coefficients::tapsForRate(176400)==1024);
    assert(Coefficients::tapsForRate(192000)==1024);
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
                magnitudeError=std::max(magnitudeError,error);assert(std::isfinite(error));
            }
        }
    }
    std::printf("Short FIR modal target magnitudes: worst truncation error %.6f dB; impulse peaks <64 samples\n",magnitudeError);
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
    eqConfig.tonalReference=2;
    for(double rate:{44100.,96000.}) {
        Processor twoModes(std::make_shared<Coefficients>(eqConfig,rate));twoModes.dormant=false;twoModes.blend=1;
        const double elapsed=seconds([&]{for(int n=0;n<int(rate)*10;n+=32){a.fill(.001f);twoModes.process(a.data(),2,a.data()+1,2,32,true);}});
        std::printf("%gHz, two FIR modes + 31 GEQ: %.6fs per 10s (%.3f%% of one core)\n",rate,elapsed,elapsed*10);
        assert(elapsed<1);
    }
    assert(modern<1); // broad real-time budget, not an unstable microbenchmark speed-ratio gate
    // Concurrent control-side allocation/publication/reclamation and callback processing.
    std::atomic<bool> done{false};std::thread audio([&]{std::array<float,64>s{};while(!done.load()){s.fill(.001f);effect.process(s.data(),2,s.data()+1,2,32);for(float x:s)assert(std::isfinite(x));}});
    for(int i=0;i<40;++i){config.spacingCM=10+i;effect.configure(config,44100);effect.enabled.store(i%2==0);}
    done=true;audio.join();
    std::puts("PASS: minimum phase, short tapered FIR, zero-buffer streaming impulse, full-matrix/NEON parity, rates, GEQ, bypass reset, CPU and live publication");
}
