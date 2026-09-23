// Sampled on the control side once a second; never called by audio callbacks.
struct DriverOutputHealth {
    struct Sample {
        let callbacks:UInt64
        let missing:UInt64
        let received:UInt64
    }
    private var previous:[Sample]=[]
    private var stalled:[Int]=[]
    private var starved:[Int]=[]
    mutating func observe(_ samples:[Sample])->Bool {
        guard samples.count==previous.count else {
            previous=samples
            stalled=Array(repeating:0,count:samples.count)
            starved=stalled
            return false
        }
        for i in samples.indices {
            let current=samples[i],old=previous[i]
            // A restarted output has new counters, not a huge unsigned delta.
            guard current.callbacks>=old.callbacks,current.missing>=old.missing,current.received>=old.received else {
                stalled[i]=0;starved[i]=0;continue
            }
            stalled[i]=current.callbacks==old.callbacks ? stalled[i]+1:0
            let lost=current.missing-old.missing,read=current.received-old.received
            // Ignore historical losses and isolated glitches. Once reception has
            // begun, two consecutive polls with >=10% loss require a fresh ring
            // mapping and output clock. Before first reception, silence is normal.
            let substantial=current.received>0 && lost>0 && Double(lost)>=0.1*(Double(lost)+Double(read))
            starved[i]=substantial ? starved[i]+1:0
        }
        previous=samples
        return stalled.contains { $0>=2 } || starved.contains { $0>=2 }
    }
}
