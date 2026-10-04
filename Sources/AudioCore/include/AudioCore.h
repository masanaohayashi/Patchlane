#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Dipole presets are synthesized on the control thread at the actual output rate.
typedef struct {
    double spacingCM,distanceCM,headCM,maxBoostDB,trimDB;
    int tonalReference;
    double geqDB[31];
} LCDipoleConfig;
typedef struct LCDipoleTest LCDipoleTest;
LCDipoleTest *lc_dipole_test_create(const LCDipoleConfig *config,double rate);
void lc_dipole_test_destroy(LCDipoleTest *effect);
void lc_dipole_test_process(LCDipoleTest *effect,float *stereo,int frames,int enabled);
int lc_dipole_latency(void);
int lc_dipole_taps(double rate);
const char *lc_dipole_backend(void);
// Control-thread-only connection diagnostic; does not start audio IO.
int lc_shared_probe(char *error,int capacity);
// Explicit diagnostic only. Generates synthetic audio on idle virtual devices.
int lc_latency_check(int argc,char **argv);
int lc_shared_silent_check(uint32_t source,uint32_t destination,int frames,double rate,double delayFrames,double seconds,char *report,int capacity);
// Experimental direct output. Lifecycle calls must be serialized. Delay is an
// explicit number of source frames; zero adds no queue or priming delay.
typedef struct LCSharedOutput LCSharedOutput;
LCSharedOutput *lc_shared_output_create(void);
void lc_shared_output_destroy(LCSharedOutput *output);
int lc_shared_output_start(LCSharedOutput *output,uint32_t device,int bus,int left,int right,int frames,double rate,double delayFrames);
int lc_shared_output_start_source(LCSharedOutput *output,uint32_t source,uint32_t device,int bus,int left,int right,int frames,double rate,double delayFrames);
// Configure source channels while stopped; levels/meters are lock-free.
int lc_shared_output_channels(LCSharedOutput *output,int left,int right);
int lc_shared_output_dipole(LCSharedOutput *output,const LCDipoleConfig *config,double rate);
void lc_shared_output_dipole_enabled(LCSharedOutput *output,int enabled);
void lc_shared_output_levels(LCSharedOutput *output,float inputGain,int routed,int muted);
float lc_shared_output_input_peak(LCSharedOutput *output);
float lc_shared_output_peak(LCSharedOutput *output);
void lc_shared_output_stop(LCSharedOutput *output);
const char *lc_shared_output_error(LCSharedOutput *output);
uint64_t lc_shared_output_callbacks(LCSharedOutput *output);
uint64_t lc_shared_output_missing_frames(LCSharedOutput *output);
uint64_t lc_shared_output_invalid_clock(LCSharedOutput *output);
int lc_shared_output_needs_reconnect(LCSharedOutput *output);
// Control thread only, serialized with start/stop. Does not replace a live map.
int lc_shared_output_refresh_source(LCSharedOutput *output);
uint64_t lc_shared_output_initial_missing_frames(LCSharedOutput *output);
uint64_t lc_shared_output_received_frames(LCSharedOutput *output);
typedef struct LCEngine LCEngine;
typedef struct LCBridge LCBridge;
// Private aggregate, one IOProc: virtual stereo bus -> physical output pair.
// Lifecycle calls must be serialized; stop restores unchanged device timing.
LCBridge *lc_bridge_create(void);
void lc_bridge_destroy(LCBridge *bridge);
int lc_bridge_start(LCBridge *bridge,uint32_t source,uint32_t destination,int bus,int left,int right,int frames,double rate);
void lc_bridge_stop(LCBridge *bridge);
const char *lc_bridge_error(LCBridge *bridge);
uint64_t lc_bridge_callbacks(LCBridge *bridge);
uint64_t lc_bridge_missing_frames(LCBridge *bridge);
uint32_t lc_bridge_min_frames(LCBridge *bridge);
uint32_t lc_bridge_max_frames(LCBridge *bridge);
float lc_bridge_peak(LCBridge *bridge);
typedef struct { uint32_t id, inputs, outputs; double rate; char name[256], uid[256]; } LCDevice;
int lc_devices(LCDevice *devices, int capacity);
LCEngine *lc_create(void);
void lc_destroy(LCEngine *e);
// Configuration and lifecycle calls must be serialized with lc_mix_read.
int lc_config_input(LCEngine *e, int index, uint32_t device, int left, int right);
int lc_config_output(LCEngine *e, int bus, uint32_t device);
int lc_config_output_channels(LCEngine *e,int bus,int left,int right);
int lc_output_dipole(LCEngine *e,int bus,const LCDipoleConfig *config,double rate);
void lc_output_dipole_enabled(LCEngine *e,int bus,int enabled);
void lc_input_gain(LCEngine *e, int index, float gain);
void lc_output_gain(LCEngine *e, int bus, float gain);
void lc_route(LCEngine *e, int input, int bus, int enabled);
int lc_config_timing(LCEngine *e, int frames, double rate);
int lc_config_delay(LCEngine *e,int sourceFrames);
uint32_t lc_output_callback_frames(LCEngine *e, int bus);
int lc_start(LCEngine *e);
void lc_stop(LCEngine *e);
const char *lc_error(LCEngine *e);
float lc_input_peak(LCEngine *e, int index);
float lc_output_peak(LCEngine *e, int index);
// Offline pull for diagnostics; not a network streaming interface.
void lc_mix_read(LCEngine *e, int bus, float *stereo, int frames, double rate);
void lc_mix_reset(LCEngine *e, int bus);
// Hardware-independent test entry point, before starting devices.
void lc_test_feed(LCEngine *e, int input, const float *stereo, int frames, double rate);
#ifdef __cplusplus
}
#endif
