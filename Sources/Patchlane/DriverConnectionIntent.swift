// Main-thread connection intent, separate from asynchronous device operations.
// A user stop revokes recovery; only interruptions of a requested connection
// may reconnect. Time is supplied by the caller for deterministic tests.
struct DriverConnectionIntent {
    private(set) var wanted=false
    private(set) var sleeping=false
    private(set) var recovering=false
    private var retryAt:Double=0
    mutating func request() { wanted=true;recovering=false }
    mutating func stop() { wanted=false;recovering=false }
    mutating func interrupt(now:Double) {
        guard wanted else { return }
        recovering=true;retryAt=now+2
    }
    mutating func sleep(now:Double) { sleeping=true;interrupt(now:now) }
    mutating func wake(now:Double) { sleeping=false;retryAt=now+2 }
    func shouldRetry(now:Double)->Bool { wanted && recovering && !sleeping && now>=retryAt }
    mutating func attempting(now:Double) { retryAt=now+5 }
    mutating func succeeded() { recovering=false }
    mutating func failed() { if !recovering { wanted=false } }
}
