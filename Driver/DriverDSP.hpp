#pragma once
#include "../Shared/AudioRing.hpp"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace lcd {
#ifndef LCD_CHANNELS
#define LCD_CHANNELS 8
#endif
constexpr uint32_t Channels=LCD_CHANNELS, Buses=4, Capacity=65536, MaxFrames=8192;
static_assert(Channels==2 || Channels==8);
constexpr uint32_t ConfigVersion=1;
// Wire format carried in CFData, marshalled as a CFPropertyList by the HAL.
struct Config {
    uint32_t version=ConfigVersion;
    float inputGain[Buses]{1,1,1,1};
    float outputGain[Buses]{1,1,1,1};
    uint32_t route[Buses][Buses]{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
};
static_assert(sizeof(Config)==100);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
inline bool valid(const Config &c) {
    if(c.version!=ConfigVersion) return false;
    for(auto g:c.inputGain) if(!std::isfinite(g)||g<0||g>64) return false;
    for(auto g:c.outputGain) if(!std::isfinite(g)||g<0||g>1) return false;
    for(auto &row:c.route) for(auto v:row) if(v>1) return false;
    return true;
}
// One control publisher (serialized on the non-audio thread), one audio writer.
// SC atomics make snapshots data-race free even if a reader is lapped. Readers
// never spin: a conflicting update uses the preceding complete configuration.
class Controls {
    std::mutex mutex;
    Config current;
    std::atomic<uint64_t> sequence{0};
    std::array<std::atomic<uint32_t>,sizeof(Config)/4> words{};
public:
    Controls() { publish(current); }
    bool publish(const Config &c) {
        if(!valid(c)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        uint32_t bytes[sizeof(Config)/4]; std::memcpy(bytes,&c,sizeof(c));
        sequence.fetch_add(1);
        for(size_t i=0;i<words.size();++i) words[i].store(bytes[i]);
        sequence.fetch_add(1); current=c;
        return true;
    }
    Config get() { std::lock_guard<std::mutex> lock(mutex); return current; }
    bool snapshot(Config &c) const {
        const auto before=sequence.load(); if(before&1) return false;
        uint32_t bytes[sizeof(Config)/4];
        for(size_t i=0;i<words.size();++i) bytes[i]=words[i].load();
        if(sequence.load()!=before) return false;
        std::memcpy(&c,bytes,sizeof(c)); return true;
    }
};
// Single WriteMix producer, multiple ReadInput consumers. Capacity is retention
// only: reads address an exact timestamp; no queue priming or latency offset.
class DSP {
    lcshared::AudioRing localRing;
    lcshared::AudioRing *ring=&localRing;
    uint64_t epoch=1; // Only changed when HAL has stopped IO.
    Config config;
    float gain[Buses][Buses]{};
    bool first=true;
public:
    // Mapping ownership stays with the control thread. Attach/detach only with
    // HAL IO stopped; retain the mapping until every reader has disconnected.
    void attachRing(lcshared::AudioRing *shared) { ring->invalidateClock();ring=shared?shared:&localRing;reset(); }
    const lcshared::AudioRing &audioRing() const { return *ring; }
    uint64_t generation() const { return epoch; }
    Controls controls;
    std::atomic<uint64_t> missing{0}, written{0}, readFrames{0};
    void reset() { ring->invalidateClock();++epoch; first=true; missing=0; written=0; readFrames=0; }
    void write(int64_t time,const float *source,uint32_t frames,double rate,float master=1.f,uint64_t hostTime=0,double ticksPerFrame=0) {
        controls.snapshot(config);
        float target[Buses][Buses];
        for(unsigned i=0;i<Buses;++i) for(unsigned b=0;b<Buses;++b) {
            target[i][b]=config.route[i][b]*config.inputGain[i]*config.outputGain[b]*master;
            if(first) gain[i][b]=target[i][b];
        }
        first=false;
        // Dezipper changes amplitude without delaying samples; no lookahead.
        const float slew=1-std::exp(-1.f/(float(rate)*0.005f));
        for(unsigned f=0;f<frames;++f) {
            float mixed[Channels]{};
            for(unsigned i=0;i<Channels/2;++i) {
                float l=source[f*Channels+i*2],r=source[f*Channels+i*2+1];
                if(!std::isfinite(l)) l=0; if(!std::isfinite(r)) r=0;
                for(unsigned b=0;b<Channels/2;++b) {
                    gain[i][b]+=(target[i][b]-gain[i][b])*slew;
                    mixed[b*2]+=l*gain[i][b]; mixed[b*2+1]+=r*gain[i][b];
                }
            }
            lcshared::Snapshot sample;
            sample.generation=epoch;sample.sampleTime=time+f;
            sample.hostTime=hostTime?hostTime+uint64_t(std::llround(double(f)*ticksPerFrame)):0;
            for(unsigned c=0;c<Channels;++c) sample.samples[c]=std::clamp(mixed[c],-1.f,1.f);
            ring->write(sample);
        }
        if(frames) ring->publishClock({epoch,hostTime,time,time+frames,ticksPerFrame});
        written.fetch_add(frames,std::memory_order_relaxed);
    }
    void read(int64_t time,float *dest,uint32_t frames) {
        uint64_t lost=0;
        for(unsigned f=0;f<frames;++f) {
            lcshared::Snapshot sample;
            const bool ok=ring->read(time+f,epoch,sample);
            if(!ok) ++lost;
            for(unsigned c=0;c<Channels;++c) dest[f*Channels+c]=sample.samples[c];
        }
        missing.fetch_add(lost,std::memory_order_relaxed);
        readFrames.fetch_add(frames,std::memory_order_relaxed);
    }
};
}
