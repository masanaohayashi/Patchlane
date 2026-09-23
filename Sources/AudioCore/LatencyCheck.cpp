// Use the identical generator/detector in the CLI tool and signed app. Broker
// authentication deliberately only permits the installed app to acquire rings.
#define PATCHLANE_EMBED_LATENCY_CHECK
#include "../../Tools/loopback-latency.cpp"
extern "C" int lc_latency_check(int argc,char **argv) { return runLoopbackLatency(argc,argv); }
