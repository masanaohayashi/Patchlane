// Shared start gate, including delayed permission and wake callbacks.
struct AudioModeLifecycle {
    var enabled=false
    var dedicated=false
    var sleeping=false
    var shuttingDown=false
    var shouldRunMixer:Bool { enabled && !dedicated && !sleeping && !shuttingDown }
}
