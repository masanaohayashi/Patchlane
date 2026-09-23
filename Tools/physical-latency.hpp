// Explicit line-output 1 -> line-input 1 benchmark. Samples remain in memory.
namespace {
struct PhysicalCapture {
    static constexpr unsigned capacity=262144;
    std::vector<float> samples=std::vector<float>(capacity);
    std::vector<double> times=std::vector<double>(capacity);
    unsigned used=0;
    uint32_t random=0x12345678;
    double ticks=0;
    bool generate=false,bad=false;
};
OSStatus physicalCallback(AudioDeviceID,const AudioTimeStamp*,const AudioBufferList *in,const AudioTimeStamp *it,
                          AudioBufferList *out,const AudioTimeStamp *ot,void *ref) {
    auto &c=*static_cast<PhysicalCapture*>(ref);
    if(out) for(unsigned b=0;b<out->mNumberBuffers;++b) if(out->mBuffers[b].mData)
        std::memset(out->mBuffers[b].mData,0,out->mBuffers[b].mDataByteSize);
    auto list=c.generate?out:const_cast<AudioBufferList*>(in);
    auto stamp=c.generate?ot:it;
    if(!stamp||!(stamp->mFlags&kAudioTimeStampHostTimeValid)) { c.bad=true; return noErr; }
    const double scalar=(stamp->mFlags&kAudioTimeStampRateScalarValid)?stamp->mRateScalar:1;
    if(!std::isfinite(scalar)||scalar<=0) { c.bad=true; return noErr; }
    for(unsigned f=0;f<count(list);++f) {
        float *p=channel(list,0,f);
        if(!p||c.used>=c.capacity) { c.bad=true; break; }
        if(c.generate) {
            c.random^=c.random<<13; c.random^=c.random>>17; c.random^=c.random<<5;
            *p=(double(c.random)/4294967295.0*2-1)*0.025; // peak -32 dBFS
        }
        c.samples[c.used]=*p;
        c.times[c.used]=double(stamp->mHostTime)+f*c.ticks*scalar;
        ++c.used;
    }
    return noErr;
}
struct Correlation { int offset=0; double score=0,gain=0; };
Correlation correlate(const PhysicalCapture &send,const PhysicalCapture &receive,unsigned start,unsigned length,int radius,int center=0) {
    Correlation best;
    for(int lag=center-radius;lag<=center+radius;++lag) {
        if(int(start)+lag<0||start+length>send.used||int(start)+lag+int(length)>int(receive.used)) continue;
        double xy=0,xx=0,yy=0;
        for(unsigned n=0;n<length;++n) {
            double x=send.samples[start+n],y=receive.samples[int(start+n)+lag];
            xy+=x*y;xx+=x*x;yy+=y*y;
        }
        double score=(xx>0&&yy>0)?xy/std::sqrt(xx*yy):0;
        if(std::abs(score)>std::abs(best.score)) best={lag,score,xx>0?xy/xx:0};
    }
    return best;
}
int physicalSelfTest() {
    auto send=std::make_unique<PhysicalCapture>();auto receive=std::make_unique<PhysicalCapture>();
    send->generate=true;send->ticks=1;
    float data[32]{};AudioBufferList out{1,{{1,sizeof(data),data}}};
    AudioTimeStamp stamp{};stamp.mFlags=kAudioTimeStampHostTimeValid;
    for(unsigned n=0;n<4096;n+=32) { stamp.mHostTime=10000+n;physicalCallback(0,nullptr,nullptr,nullptr,&out,&stamp,send.get()); }
    for(int delay:{-31,0,65,1024}) {
        receive->used=send->used;
        std::fill(receive->samples.begin(),receive->samples.end(),0);
        for(unsigned n=128;n<3000;++n) receive->samples[int(n)+delay]=send->samples[n]*-0.2f;
        auto result=correlate(*send,*receive,512,2048,100,delay==1024?1024:0);
        if(result.offset!=delay||std::abs(result.score+1)>1e-6) return 1;
    }
    std::puts("PASS physical correlation: known delays and inverted attenuation");return 0;
}
int runPhysicalLatency(int argc,char **argv) {
    if(argc==3&&std::strcmp(argv[2],"--selftest")==0) return physicalSelfTest();
    if(argc!=7) { std::fprintf(stderr,"Usage: --physical zoomUID direct|shared|aggregate frames rate delayFrames\n");return 2; }
    try {
        const std::string mode=argv[3];unsigned frames=std::stoul(argv[4]);double rate=std::stod(argv[5]),delay=std::stod(argv[6]);
        if((mode!="direct"&&mode!="shared"&&mode!="aggregate")||(frames!=32&&frames!=64&&frames!=128)||
           (rate!=44100&&rate!=48000)||!std::isfinite(delay)||delay<0||delay>=65536) throw std::runtime_error("Invalid physical benchmark parameters");
        std::vector<LCDevice> devices(lc_devices(nullptr,0)+16);int n=lc_devices(devices.data(),int(devices.size()));
        AudioDeviceID zoom=0,source=0;
        for(int i=0;i<n&&i<int(devices.size());++i) {
            if(std::strcmp(devices[i].uid,argv[2])==0&&std::strstr(devices[i].name,"AMS-24")&&devices[i].inputs>=1&&devices[i].outputs>=2) zoom=devices[i].id;
            if(std::strcmp(devices[i].uid,"audio.patchlane.virtual8")==0) source=devices[i].id;
        }
        if(!zoom||!source) throw std::runtime_error("ZOOM AMS-24 or dedicated virtual driver unavailable");
        auto send=std::make_unique<PhysicalCapture>(),receive=std::make_unique<PhysicalCapture>();
        mach_timebase_info_data_t tb{};mach_timebase_info(&tb);double hz=1e9*double(tb.denom)/tb.numer;
        send->generate=true;send->ticks=receive->ticks=hz/rate;
        DeviceSession physicalSession,sourceSession;
        physicalSession.prepare(zoom,frames,rate);
        if(mode!="direct") sourceSession.prepare(source,frames,rate);
        auto shared=std::unique_ptr<LCSharedOutput,decltype(&lc_shared_output_destroy)>(lc_shared_output_create(),lc_shared_output_destroy);
        auto bridge=std::unique_ptr<LCBridge,decltype(&lc_bridge_destroy)>(lc_bridge_create(),lc_bridge_destroy);
        // Separate IOProcs: receive callback zeros only its own output contribution.
        AudioDeviceIOProcID generator=nullptr;
        struct GeneratorGuard { AudioDeviceID device;AudioDeviceIOProcID &proc;~GeneratorGuard(){if(proc){AudioDeviceStop(device,proc);AudioDeviceDestroyIOProcID(device,proc);}} } guard{zoom,generator};
        if(mode=="shared"&&lc_shared_output_start(shared.get(),zoom,0,0,1,frames,rate,delay)) throw std::runtime_error(lc_shared_output_error(shared.get()));
        if(mode=="aggregate"&&lc_bridge_start(bridge.get(),source,zoom,0,0,1,frames,rate)) throw std::runtime_error(lc_bridge_error(bridge.get()));
        physicalSession.start(physicalCallback,receive.get());
        if(mode=="direct") {
            if(AudioDeviceCreateIOProcID(zoom,physicalCallback,send.get(),&generator)||AudioDeviceStart(zoom,generator)) throw std::runtime_error("Generator start failed");
        } else sourceSession.start(physicalCallback,send.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto before=lc_shared_output_missing_frames(shared.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        auto missing=lc_shared_output_missing_frames(shared.get())-before;
        if(generator){AudioDeviceStop(zoom,generator);AudioDeviceDestroyIOProcID(zoom,generator);generator=nullptr;}
        sourceSession.stop();physicalSession.stop();lc_shared_output_stop(shared.get());lc_bridge_stop(bridge.get());
        if(send->bad||receive->bad||send->used<rate*2||receive->used<rate*2) throw std::runtime_error("Insufficient valid capture");
        double inputPeak=0,inputEnergy=0;
        for(unsigned i=0;i<receive->used;++i) {inputPeak=std::max(inputPeak,std::abs(double(receive->samples[i])));inputEnergy+=double(receive->samples[i])*receive->samples[i];}
        std::printf("input_peak=%.8f input_rms=%.8f send_frames=%u receive_frames=%u\n",inputPeak,std::sqrt(inputEnergy/receive->used),send->used,receive->used);
        std::vector<double> delays;
        bool valid=true;
        for(double second:{0.5,1.0,1.5}) {
            unsigned start=unsigned(second*rate);
            const int aligned=int(std::lower_bound(receive->times.begin(),receive->times.begin()+receive->used,send->times[start])-receive->times.begin())-int(start);
            auto c=correlate(*send,*receive,start,4096,4096,aligned);
            double ms=(receive->times[int(start)+c.offset]-send->times[start])*1000/hz;
            std::printf("window=%.1fs correlation=%.6f gain=%.6f lag=%d timestamp_delta_ms=%.6f\n",second,c.score,c.gain,c.offset,ms);
            if(std::abs(c.score)<0.35||std::abs(c.offset-aligned)==4096||ms<0||ms>100) valid=false;
            delays.push_back(ms);
        }
        std::printf("physical_path=%s frames=%u rate=%.0f explicit_delay=%.0f steady_missing=%llu\n",mode.c_str(),frames,rate,delay,(unsigned long long)missing);
        if(!valid) throw std::runtime_error("No reliable line-loopback correlation; no latency claim");
        report("line_loopback_timestamp_delta",delays);
        std::puts("Includes analog path and HAL timestamp accounting; not an independent oscilloscope measurement.");
        return missing?1:0;
    } catch(const std::exception &e){std::fprintf(stderr,"ERROR physical: %s\n",e.what());return 1;}
}
}
