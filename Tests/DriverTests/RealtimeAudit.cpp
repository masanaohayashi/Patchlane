#include "RealtimeAudit.hpp"
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <time.h>
static thread_local bool realtime=false;
extern "C" void patchlane_audit_enter() { realtime=true; }
extern "C" void patchlane_audit_leave() { realtime=false; }
static void check() { if(realtime) _exit(86); }
#define INTERPOSE(replacement,original) \
__attribute__((used)) static struct { const void *replacement; const void *original; } \
entry_##original __attribute__((section("__DATA,__interpose"))) = { (const void *)&replacement, (const void *)&original }
static void *audit_malloc(size_t n) { check();return malloc(n); }
static void *audit_calloc(size_t n,size_t s) { check();return calloc(n,s); }
static void *audit_realloc(void *p,size_t n) { check();return realloc(p,n); }
static void audit_free(void *p) { check();free(p); }
static int audit_lock(pthread_mutex_t *p) { check();return pthread_mutex_lock(p); }
static int audit_trylock(pthread_mutex_t *p) { check();return pthread_mutex_trylock(p); }
static int audit_wait(pthread_cond_t *c,pthread_mutex_t *p) { check();return pthread_cond_wait(c,p); }
static kern_return_t audit_semaphore(semaphore_t s) { check();return semaphore_wait(s); }
static int audit_nanosleep(const struct timespec *r,struct timespec *s) { check();return nanosleep(r,s); }
static unsigned audit_sleep(unsigned s) { check();return sleep(s); }
static int audit_usleep(useconds_t s) { check();return usleep(s); }
static void audit_sync(dispatch_queue_t q,void *c,dispatch_function_t f) { check();dispatch_sync_f(q,c,f); }
INTERPOSE(audit_malloc,malloc);INTERPOSE(audit_calloc,calloc);INTERPOSE(audit_realloc,realloc);INTERPOSE(audit_free,free);
INTERPOSE(audit_lock,pthread_mutex_lock);INTERPOSE(audit_trylock,pthread_mutex_trylock);INTERPOSE(audit_wait,pthread_cond_wait);
INTERPOSE(audit_semaphore,semaphore_wait);INTERPOSE(audit_nanosleep,nanosleep);INTERPOSE(audit_sleep,sleep);INTERPOSE(audit_usleep,usleep);
INTERPOSE(audit_sync,dispatch_sync_f);
