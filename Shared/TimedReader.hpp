#pragma once
#include "AudioRing.hpp"
#include "InterpolationKernel.hpp"
#include <limits>

namespace lcshared {
// One instance per output callback. No queue, allocation, lock, IPC or retry.
// Output ticks/frame must describe the physical clock, not merely nominal Hz.
// A supplied kernel enables band-limited interpolation; null retains the legacy
// linear path for baseline measurements.
class TimedReader {
    const AudioRing &ring;
    const InterpolationKernel *kernel;
    ClockSnapshot anchor{};
    double firstOffset=0,step=0;
    bool ready=false;
public:
    TimedReader(const AudioRing &source,uint64_t outputHostTime,
                double outputTicksPerFrame,double delaySourceFrames,const InterpolationKernel *interpolation=nullptr):ring(source),kernel(interpolation) {
        if(!outputHostTime||!std::isfinite(outputTicksPerFrame)||outputTicksPerFrame<=0||
           !std::isfinite(delaySourceFrames)||delaySourceFrames<0||delaySourceFrames>=Capacity||
           !ring.readClock(anchor)) return;
        // Subtract integer ticks before conversion: uptime must not lose the
        // fractional position within a sample through subtraction of doubles.
        const double delta=outputHostTime>=anchor.hostTime?
            double(outputHostTime-anchor.hostTime):-double(anchor.hostTime-outputHostTime);
        firstOffset=delta/anchor.ticksPerFrame-delaySourceFrames;
        step=outputTicksPerFrame/anchor.ticksPerFrame;
        ready=std::isfinite(firstOffset)&&std::isfinite(step)&&step>0&&(!kernel||kernel->supports(step));
    }
    bool rateChanged() const { return kernel&&step>0&&!kernel->supports(step); }
    bool read(uint32_t frame,unsigned bus,float &left,float &right) const {
        left=right=0;
        if(!ready||bus>=Channels/2) return false;
        const double offset=firstOffset+double(frame)*step;
        // Reject distant/stale clocks before any float-to-integer conversion.
        if(!std::isfinite(offset)||offset<=-double(Capacity)||offset>=double(Capacity)) return false;
        const auto whole=static_cast<int64_t>(std::floor(offset));
        if((whole>0&&anchor.sampleTime>std::numeric_limits<int64_t>::max()-whole)||
           (whole<0&&anchor.sampleTime<std::numeric_limits<int64_t>::min()-whole)) return false;
        const int64_t time=anchor.sampleTime+whole;
        const double fraction=offset-double(whole);
        // The writer may finish more frames during this callback. The frame's
        // timestamp, generation and sequence establish availability; an old
        // block-end snapshot must not reject data that has since arrived.
        if(kernel && (fraction>0 || kernel->nominalStep>1.01)) {
            if(time<std::numeric_limits<int64_t>::min()+kernel->before ||
               time>std::numeric_limits<int64_t>::max()-kernel->after) return false;
            const double phase=fraction*InterpolationKernel::Phases;
            const int p=int(phase);const double blend=phase-p;
            double sumLeft=0,sumRight=0;
            for(int tap=0;tap<kernel->taps;++tap) {
                float sampleLeft=0,sampleRight=0;
                if(!ring.readStereo(time+tap-kernel->before,anchor.generation,bus,sampleLeft,sampleRight)) return false;
                const auto &a=kernel->table[p];const auto &b=kernel->table[p+1];
                const double weight=a[tap]+blend*(b[tap]-a[tap]);
                sumLeft+=weight*sampleLeft;sumRight+=weight*sampleRight;
            }
            if(ring.clock.generation.load()!=anchor.generation) return false;
            left=float(sumLeft);right=float(sumRight);return true;
        }
        Snapshot a,b;
        if(!ring.read(time,anchor.generation,a)) return false;
        // At an exact integer position the next frame need not exist.
        if(fraction>0) {
            if(time==std::numeric_limits<int64_t>::max()||
               !ring.read(time+1,anchor.generation,b)) return false;
        } else b=a;
        if(ring.clock.generation.load()!=anchor.generation) return false;
        const auto ch=bus*2;
        left=float(a.samples[ch]+fraction*(b.samples[ch]-a.samples[ch]));
        right=float(a.samples[ch+1]+fraction*(b.samples[ch+1]-a.samples[ch+1]));
        return true;
    }
};
}
