#include "DipoleDSP.hpp"
struct LCDipoleTest { patchdipole::Effect effect; };
extern "C" {
LCDipoleTest *lc_dipole_test_create(const LCDipoleConfig *c,double rate) {
    if(!c)return nullptr;try { auto e=std::make_unique<LCDipoleTest>();e->effect.configure(*c,rate);return e.release(); }catch(...){return nullptr;}
}
void lc_dipole_test_destroy(LCDipoleTest *e){delete e;}
void lc_dipole_test_process(LCDipoleTest *e,float *p,int n,int enabled){if(e&&p&&n>0){e->effect.enabled.store(enabled!=0);e->effect.process(p,2,p+1,2,n);}}
int lc_dipole_latency(){return 0;}
int lc_dipole_taps(double rate){return std::isfinite(rate)&&rate>0?patchdipole::Coefficients::tapsForRate(rate):0;}
const char *lc_dipole_backend(){
#if defined(__aarch64__) && !defined(PATCHLANE_DIPOLE_SCALAR)
    return "NEON direct FIR, 2 modal paths, zero block latency";
#else
    return "Direct FIR, 2 modal paths, zero block latency";
#endif
}
}
