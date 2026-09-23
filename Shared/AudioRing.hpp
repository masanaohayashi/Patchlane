#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <type_traits>

namespace lcshared {
constexpr uint64_t Magic=0x4c43325348415245ull;
constexpr uint32_t Version=2, Channels=8, Capacity=65536;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);

// No pointers, mutexes, allocators or process-local ownership in the mapping.
// One writer. Readers may map read-only and never retry/spin in an audio callback.
struct alignas(64) Frame {
    std::atomic<uint64_t> sequence{0}, generation{0};
    std::atomic<int64_t> sampleTime{0};
    std::atomic<uint64_t> hostTime{0};
    std::array<std::atomic<float>,Channels> samples{};
};
static_assert(sizeof(Frame)==64);
struct Snapshot {
    uint64_t generation=0,hostTime=0;
    int64_t sampleTime=0;
    float samples[Channels]{};
};
// Anchor is the first frame of the most recently completed block. endTime is
// exclusive. Invalidation and publication share the single producer contract.
struct ClockSnapshot {
    uint64_t generation=0,hostTime=0;
    int64_t sampleTime=0,endTime=0;
    double ticksPerFrame=0;
};
struct alignas(64) ClockState {
    std::atomic<uint64_t> sequence{0},generation{0},hostTime{0};
    std::atomic<int64_t> sampleTime{0},endTime{0};
    std::atomic<double> ticksPerFrame{0};
};
static_assert(sizeof(ClockState)==64);
struct alignas(64) AudioRing {
    // Immutable after initialization, before the mapping is sent to a reader.
    uint64_t magic=Magic;
    uint32_t version=Version,channels=Channels,capacity=Capacity,frameBytes=sizeof(Frame);
    uint64_t mappedBytes=sizeof(AudioRing);
    std::array<uint64_t,4> reserved{};
    ClockState clock;
    std::array<Frame,Capacity> frames{};
    void publishClock(const ClockSnapshot &value) {
        clock.sequence.fetch_add(1);
        clock.generation.store(value.generation);clock.hostTime.store(value.hostTime);
        clock.sampleTime.store(value.sampleTime);clock.endTime.store(value.endTime);
        clock.ticksPerFrame.store(value.ticksPerFrame);
        clock.sequence.fetch_add(1);
    }
    void invalidateClock() { publishClock({}); }
    bool readClock(ClockSnapshot &result) const {
        result={};
        const auto before=clock.sequence.load();
        if(!before||(before&1)) return false;
        ClockSnapshot candidate;
        candidate.generation=clock.generation.load();candidate.hostTime=clock.hostTime.load();
        candidate.sampleTime=clock.sampleTime.load();candidate.endTime=clock.endTime.load();
        candidate.ticksPerFrame=clock.ticksPerFrame.load();
        if(clock.sequence.load()!=before||!candidate.generation||!candidate.hostTime||
           candidate.endTime<=candidate.sampleTime||!std::isfinite(candidate.ticksPerFrame)||
           candidate.ticksPerFrame<=0) return false;
        result=candidate;return true;
    }
    bool compatible(uint64_t bytes) const {
        return bytes>=sizeof(AudioRing)&&magic==Magic&&version==Version&&channels==Channels&&
            capacity==Capacity&&frameBytes==sizeof(Frame)&&mappedBytes==sizeof(AudioRing);
    }
    // Called only by the producer, never concurrently with another producer.
    void write(const Snapshot &sample) {
        auto &frame=frames[uint64_t(sample.sampleTime)%Capacity];
        frame.sequence.fetch_add(1);
        frame.generation.store(sample.generation);frame.sampleTime.store(sample.sampleTime);
        frame.hostTime.store(sample.hostTime);
        for(unsigned ch=0;ch<Channels;++ch) frame.samples[ch].store(sample.samples[ch]);
        frame.sequence.fetch_add(1);
    }
    // Stereo consumers need neither the other buses nor per-frame host time.
    // Keep the same timestamp/generation/seqlock checks as the full snapshot.
    bool readStereo(int64_t time,uint64_t generation,unsigned bus,float &left,float &right) const {
        left=right=0;
        if(bus>=Channels/2) return false;
        const auto &frame=frames[uint64_t(time)%Capacity];
        const auto sequence=frame.sequence.load();
        if(!sequence||(sequence&1)||frame.generation.load()!=generation||frame.sampleTime.load()!=time) return false;
        const float a=frame.samples[bus*2].load(),b=frame.samples[bus*2+1].load();
        if(frame.sequence.load()!=sequence) return false;
        left=a;right=b;return true;
    }
    bool read(int64_t time,uint64_t generation,Snapshot &result) const {
        const auto &frame=frames[uint64_t(time)%Capacity];
        const auto sequence=frame.sequence.load();
        if(!sequence||(sequence&1)||frame.generation.load()!=generation||frame.sampleTime.load()!=time) { result={};return false; }
        Snapshot candidate;candidate.generation=generation;candidate.sampleTime=time;candidate.hostTime=frame.hostTime.load();
        for(unsigned ch=0;ch<Channels;++ch) candidate.samples[ch]=frame.samples[ch].load();
        if(frame.sequence.load()!=sequence) { result={};return false; }
        result=candidate;return true;
    }
};
static_assert(std::is_standard_layout_v<AudioRing>);
static_assert(offsetof(AudioRing,clock)==64);
static_assert(offsetof(AudioRing,frames)==128);
}
