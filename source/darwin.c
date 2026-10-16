/**
 * @file darwin.c
 * @brief iOS Darwin libc, libgcc, and pthread emulation layer over Vita newlib.
 * @details Implements ARMv6 softfp Darwin system calls, POSIX threads with SceUID safety,
 *          virtual filesystem path translation to ux0:data/, high-resolution timers,
 *          and mathematical builtins.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"

// ---------------------------------------------------------------- paths

// Paths created by the game (NSHomeDirectory, bundle) are redirected to DATA_PATH.
const char *darwin_path(const char *in, char *out, size_t outlen) {
    if (!strncmp(in, "ux0:", 4) || !strncmp(in, "ur0:", 4) || !strncmp(in, "app0:", 5)) {
        snprintf(out, outlen, "%s", in);
        return out;
    }
    while (*in == '/')
        in++;
    snprintf(out, outlen, DATA_PATH "%s", in);
    return out;
}

static FILE *d_fopen(const char *path, const char *mode) {
    char p[512];
    FILE *f = fopen(darwin_path(path, p, sizeof(p)), mode);
    l_debug("fopen(%s, %s) -> %p", p, mode, f);
    return f;
}

static int d_mkdir(const char *path, int mode) {
    char p[512];
    return mkdir(darwin_path(path, p, sizeof(p)), mode);
}

// ---------------------------------------------------------------- stdio / errno / _chk

static FILE *d_stderrp, *d_stdinp;

static int *d___error(void) { return &errno; }

static void *d___memcpy_chk(void *d, const void *s, size_t n, size_t dn) { return memcpy(d, s, n); }
static char *d___strcpy_chk(char *d, const char *s, size_t dn) { return strcpy(d, s); }

// ---------------------------------------------------------------- printf with Darwin ABI (darwin_printf.c)

static int d_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, INT_MAX, fmt, &w);
    va_end(ap);
    return r;
}
static int d___sprintf_chk(char *buf, int flag, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    words w = {&ap, NULL};
    int r = darwin_vformat(buf, len, fmt, &w);
    va_end(ap);
    return r;
}
static int d_vsprintf(char *buf, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, INT_MAX, fmt, &w);
}
static int d_vsnprintf(char *buf, size_t size, const char *fmt, const uint32_t *ap) {
    words w = {NULL, ap};
    return darwin_vformat(buf, size, fmt, &w);
}

static void d___assert_rtn(const char *func, const char *file, int line, const char *expr) {
    l_error("guest assertion failed: %s (%s:%d %s)", expr, file, line, func);
}

// NSLog(@"format", ...): format is a CFString (C string pointer at +8).
static void d_NSLog(void *cfstr, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, cfstr);
    words w = {&ap, NULL};
    darwin_vformat(buf, sizeof(buf), cfstr ? *(const char **)((uint8_t *)cfstr + 8) : "(nil)", &w);
    va_end(ap);
    l_info("NSLog: %s", buf);
}

static int qsort_guest_cmp;
static int qsort_tramp(const void *a, const void *b) {
    return (int)call_guest((void *)qsort_guest_cmp, (uint32_t)a, (uint32_t)b, 0, 0);
}
static void d_qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    int prev = qsort_guest_cmp; // nested qsort: restored upon return
    qsort_guest_cmp = (int)cmp;
    qsort(base, n, sz, qsort_tramp);
    qsort_guest_cmp = prev;
}

// ---------------------------------------------------------------- time / system

static uint64_t d_mach_absolute_time(void) { return sceKernelGetProcessTimeWide(); }

typedef struct { uint32_t numer, denom; } mach_timebase_info_t;
static int d_mach_timebase_info(mach_timebase_info_t *info) {
    info->numer = 1000; // mach_absolute_time is in microseconds
    info->denom = 1;
    return 0;
}

static int d_usleep(unsigned us) {
    sceKernelDelayThread(us);
    return 0;
}

static int d_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    if (!strcmp(name, "hw.machine")) {
        const char *model = "iPhone2,1"; // iPhone 3GS: game selects capable GLES1 texture profile
        size_t len = strlen(model) + 1;
        if (oldp && *oldlenp >= len)
            memcpy(oldp, model, len);
        *oldlenp = len;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static void d_exit(int code) {
    l_info("guest invoked exit(%d)", code);
    sceKernelExitProcess(0);
}

// ---------------------------------------------------------------- networking (offline stub)

static int d_net_fail(void) {
    errno = ENETDOWN;
    return -1;
}
static void *d_gethostbyname(const char *n) { return NULL; }
static int d_select(int n, void *r, void *w, void *e, void *t) { return 0; }

// ---------------------------------------------------------------- pthreads

// Darwin: pthread_mutex_t = {long sig; char opaque[40]}. Vita mutex stored in opaque.
// Darwin PTHREAD_MUTEX_INITIALIZER signature: 0x32AAABA7 (lazy initialized on first use).
#define DARWIN_MUTEX_SIG_INIT 0x32AAABA7
#define ML_MUTEX_SIG 0x4D4C5458
#define ML_MUTEXATTR_SIG 0x4D4C5441
typedef struct { uint32_t sig; pthread_mutex_t m; } d_mutex;
typedef struct { uint32_t sig; int type; } d_mutexattr;

static int d_pthread_mutexattr_init(d_mutexattr *a) {
    a->sig = ML_MUTEXATTR_SIG;
    a->type = 0;
    return 0;
}
static int d_pthread_mutexattr_settype(d_mutexattr *a, int type) {
    a->type = type; // Darwin: 0 normal, 1 errorcheck, 2 recursive
    return 0;
}
static int d_pthread_mutex_init(d_mutex *m, const d_mutexattr *a) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (a && a->sig == ML_MUTEXATTR_SIG)
        pthread_mutexattr_settype(&attr, a->type == 2 ? PTHREAD_MUTEX_RECURSIVE
                                         : a->type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    int r = pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    m->sig = ML_MUTEX_SIG;
    return r;
}
static void mutex_lazy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG) {
        if (m->sig != DARWIN_MUTEX_SIG_INIT)
            l_error("uninitialized mutex (sig 0x%08X) at %p", (unsigned)m->sig, m);
        d_pthread_mutex_init(m, NULL);
    }
}
static int d_pthread_mutex_lock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_lock(&m->m);
}
static int d_pthread_mutex_unlock(d_mutex *m) {
    mutex_lazy(m);
    return pthread_mutex_unlock(&m->m);
}
static int d_pthread_mutex_destroy(d_mutex *m) {
    if (m->sig != ML_MUTEX_SIG)
        return 0;
    m->sig = 0;
    return pthread_mutex_destroy(&m->m);
}

typedef struct { void *fn, *arg; } guest_thread;
static void *thread_tramp(void *p) {
    guest_thread t = *(guest_thread *)p;
    free(p);
    return (void *)call_guest(t.fn, (uint32_t)t.arg, 0, 0, 0);
}
static int d_pthread_create(pthread_t *thr, const void *darwin_attr, void *fn, void *arg) {
    guest_thread *t = malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024); // iOS default 512 KB secondary thread stack
    int r = pthread_create(thr, &attr, thread_tramp, t);
    pthread_attr_destroy(&attr);
    l_debug("pthread_create(0x%08X) -> %d", (unsigned)fn - MACHO_SLIDE, r);
    return r;
}
static int d_pthread_detach(pthread_t t) {
    if (!t)
        return 0;
    // If handle is a Sony Vita SceUID (0x4001xxxx)
    if (((uintptr_t)t & 0xFFFF0000) == 0x40010000) {
        l_info("pthread_detach(SceUID 0x%08X) -> ok", (unsigned)(uintptr_t)t);
        return 0;
    }
    // If handle is a pointer to pte_thread_t in user memory
    uintptr_t addr = (uintptr_t)t;
    if (addr >= 0x81000000 && addr < 0x90000000 && (addr & 3) == 0) {
        uint32_t first = *(uint32_t *)addr;
        if ((first & 0xFFFF0000) == 0x40010000) {
            return pthread_detach(t);
        }
    }
    l_info("pthread_detach(%p) safely ignored", (void *)t);
    return 0;
}

static pthread_t d_pthread_self(void) {
    pthread_t p = pthread_self();
    if (p)
        return p;
    return (pthread_t)sceKernelGetThreadId();
}

static int d_pthread_setschedparam(pthread_t t, int policy, const void *param) { return 0; }

// Named semaphores (FMOD): Darwin sem_t* -> Vita SceUID.
typedef struct { SceUID id; } d_sem;
static d_sem *d_sem_open(const char *name, int oflag, ...) {
    va_list ap;
    va_start(ap, oflag);
    va_arg(ap, int); // mode
    unsigned value = va_arg(ap, unsigned);
    va_end(ap);
    d_sem *s = malloc(sizeof(*s));
    s->id = sceKernelCreateSema("ml_sem", 0, value, 0x7FFFFFFF, NULL);
    if (s->id < 0) {
        free(s);
        return (d_sem *)-1; // SEM_FAILED
    }
    return s;
}
static int d_sem_close(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    sceKernelDeleteSema(s->id);
    free(s);
    return 0;
}
static int d_sem_unlink(const char *name) { return 0; }
static int d_sem_wait(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelWaitSema(s->id, 1, NULL) < 0 ? -1 : 0;
}
static int d_sem_post(d_sem *s) {
    if (!s || s == (d_sem *)-1)
        return -1;
    return sceKernelSignalSema(s->id, 1) < 0 ? -1 : 0;
}

// ---------------------------------------------------------------- C++ / blocks / exceptions

static int d___cxa_atexit(void *f, void *a, void *d) { return 0; } // games never terminate cleanly
static int d___cxa_guard_acquire(uint8_t *g) { return *g == 0; }
static void d___cxa_guard_release(uint8_t *g) { *g = 1; }
static void d___cxa_pure_virtual(void) { fatal_error("__cxa_pure_virtual"); }
static void d_sjlj_noop(void *ctx) {}
static void d_sjlj_resume(void *ctx) { fatal_error("C++/ObjC exception thrown (_Unwind_SjLj_Resume)"); }

static uint8_t d_NSConcreteStackBlock[32];
static void d_Block_object_assign(void **dst, void *src, int flags) { *dst = src; }
static void d_Block_object_dispose(void *obj, int flags) {}

static uint8_t d_dispatch_main_q[64];
static void *d_dispatch_get_global_queue(long prio, unsigned long flags) { return d_dispatch_main_q; }
static void d_dispatch_async(void *q, void *block) { l_debug("dispatch_async ignored"); }

// ---------------------------------------------------------------- libgcc softfp (Apple *vfp)

SOFTFP static float f_add(float a, float b) { return a + b; }
SOFTFP static float f_sub(float a, float b) { return a - b; }
SOFTFP static float f_mul(float a, float b) { return a * b; }
SOFTFP static float f_div(float a, float b) { return a / b; }
SOFTFP static double d_add(double a, double b) { return a + b; }
SOFTFP static double d_sub(double a, double b) { return a - b; }
SOFTFP static double d_mul(double a, double b) { return a * b; }
// compiler-rt *vfp comparisons: return 1 if condition is true (unlike libgcc -1/0/1).
SOFTFP static int f_eq(float a, float b) { return a == b; }
SOFTFP static int f_ne(float a, float b) { return !(a == b); }
SOFTFP static int f_gt(float a, float b) { return a > b; }
SOFTFP static int f_ge(float a, float b) { return a >= b; }
SOFTFP static int f_lt(float a, float b) { return a < b; }
SOFTFP static int f_le(float a, float b) { return a <= b; }
SOFTFP static int dd_gt(double a, double b) { return a > b; }
SOFTFP static int dd_ge(double a, double b) { return a >= b; }
SOFTFP static int dd_lt(double a, double b) { return a < b; }
SOFTFP static double f_ext(float a) { return a; }
SOFTFP static float d_trunc(double a) { return (float)a; }
SOFTFP static int f_toi(float a) { return (int)a; }
SOFTFP static int d_toi(double a) { return (int)a; }
SOFTFP static long long f_toll(float a) { return (long long)a; }
SOFTFP static long long d_toll(double a) { return (long long)a; }
SOFTFP static unsigned long long f_toull(float a) { return (unsigned long long)a; }
SOFTFP static float i_tof(int a) { return (float)a; }
SOFTFP static float u_tof(unsigned a) { return (float)a; }
SOFTFP static double i_tod(int a) { return (double)a; }
SOFTFP static double ll_tod(long long a) { return (double)a; }
SOFTFP static float ll_tof(long long a) { return (float)a; }

static int i_div(int a, int b) { return a / b; }
static int i_mod(int a, int b) { return a % b; }
static unsigned u_div(unsigned a, unsigned b) { return a / b; }
static unsigned u_mod(unsigned a, unsigned b) { return a % b; }
static long long ll_div(long long a, long long b) { return a / b; }
static long long ll_mod(long long a, long long b) { return a % b; }
static long long ll_mul(long long a, long long b) { return a * b; }
static unsigned long long ull_div(unsigned long long a, unsigned long long b) { return a / b; }
static unsigned long long ull_mod(unsigned long long a, unsigned long long b) { return a % b; }
static int ll_cmp(long long a, long long b) { return a < b ? 0 : a == b ? 1 : 2; }

SOFTFP static float m_acosf(float x) { return acosf(x); }
SOFTFP static float m_cosf(float x) { return cosf(x); }
SOFTFP static float m_sinf(float x) { return sinf(x); }
SOFTFP static float m_tanf(float x) { return tanf(x); }
SOFTFP static float m_expf(float x) { return expf(x); }
SOFTFP static float m_logf(float x) { return logf(x); }
SOFTFP static float m_log10f(float x) { return log10f(x); }
SOFTFP static float m_floorf(float x) { return floorf(x); }
SOFTFP static long m_lrintf(float x) { return lrintf(x); }
SOFTFP static double m_cos(double x) { return cos(x); }
SOFTFP static double m_sin(double x) { return sin(x); }
SOFTFP static double m_floor(double x) { return floor(x); }
SOFTFP static double m_log10(double x) { return log10(x); }
SOFTFP static double m_atan2(double y, double x) { return atan2(y, x); }
SOFTFP static double m_fmod(double x, double y) { return fmod(x, y); }
SOFTFP static double m_pow(double x, double y) { return pow(x, y); }
SOFTFP static double m_ldexp(double x, int e) { return ldexp(x, e); }

// ---------------------------------------------------------------- import table

// name without Mach-O prepended "_": F(__divsi3, ...) registers "___divsi3".
#define F(name, fn) {"_" #name, (uintptr_t)(fn)}
#define D(name) {"_" #name, (uintptr_t)(name)} // identical signature in newlib
#define V(name, ptr) {"_" #name, (uintptr_t)(ptr), 1} // data symbol, not function

const import_entry darwin_imports[] = {
    // memory / strings
    D(malloc), D(calloc), D(realloc), D(free), D(memcpy), D(memmove), D(memset), D(memcmp),
    D(memchr), D(bzero), D(strlen), D(strcmp), D(strcpy), D(strncpy), D(strstr), D(strtok),
    D(atoi), D(rand), D(srand), F(sprintf, d_sprintf), F(vsprintf, d_vsprintf),
    F(vsnprintf, d_vsnprintf), D(sscanf), D(puts),
    F(qsort, d_qsort), F(_ZdlPv, free),
    F(__memcpy_chk, d___memcpy_chk), F(__strcpy_chk, d___strcpy_chk),
    F(__sprintf_chk, d___sprintf_chk), F(__assert_rtn, d___assert_rtn), F(__error, d___error),
    F(NSLog, d_NSLog),
    // stdio
    F(fopen, d_fopen), D(fclose), D(fread), D(fwrite), D(fseek), D(ftell), D(feof), D(ferror),
    D(fflush), D(fputs), F(mkdir, d_mkdir), D(close), V(__stderrp, &d_stderrp),
    V(__stdinp, &d_stdinp),
    // time / system
    D(gettimeofday), F(usleep, d_usleep), F(mach_absolute_time, d_mach_absolute_time),
    F(mach_timebase_info, d_mach_timebase_info), F(sysctlbyname, d_sysctlbyname), F(exit, d_exit),
    // network: offline stubs
    F(socket, d_net_fail), F(connect, d_net_fail), F(bind, d_net_fail), F(listen, d_net_fail),
    F(accept, d_net_fail), F(send, d_net_fail), F(recv, d_net_fail), F(setsockopt, d_net_fail),
    F(fcntl, d_net_fail), F(inet_addr, d_net_fail), F(gethostbyname, d_gethostbyname),
    F(select, d_select),
    // pthreads / semaphores
    F(pthread_create, d_pthread_create), F(pthread_detach, d_pthread_detach), F(pthread_self, d_pthread_self),
    F(pthread_setschedparam, d_pthread_setschedparam), F(pthread_mutex_init, d_pthread_mutex_init),
    F(pthread_mutex_lock, d_pthread_mutex_lock), F(pthread_mutex_unlock, d_pthread_mutex_unlock),
    F(pthread_mutex_destroy, d_pthread_mutex_destroy), F(pthread_mutexattr_init, d_pthread_mutexattr_init),
    F(pthread_mutexattr_settype, d_pthread_mutexattr_settype), F(sem_open, d_sem_open),
    F(sem_close, d_sem_close), F(sem_unlink, d_sem_unlink), F(sem_wait, d_sem_wait),
    F(sem_post, d_sem_post),
    // C++ / blocks / exceptions / GCD
    F(__cxa_atexit, d___cxa_atexit), F(__cxa_guard_acquire, d___cxa_guard_acquire),
    F(__cxa_guard_release, d___cxa_guard_release), F(__cxa_pure_virtual, d___cxa_pure_virtual),
    F(_Unwind_SjLj_Register, d_sjlj_noop), F(_Unwind_SjLj_Unregister, d_sjlj_noop),
    F(_Unwind_SjLj_Resume, d_sjlj_resume), V(_NSConcreteStackBlock, d_NSConcreteStackBlock),
    F(_Block_object_assign, d_Block_object_assign), F(_Block_object_dispose, d_Block_object_dispose),
    V(_dispatch_main_q, d_dispatch_main_q), F(dispatch_get_global_queue, d_dispatch_get_global_queue),
    F(dispatch_async, d_dispatch_async),
    // softfp helpers
    F(__addsf3vfp, f_add), F(__subsf3vfp, f_sub), F(__mulsf3vfp, f_mul), F(__divsf3vfp, f_div),
    F(__adddf3vfp, d_add), F(__subdf3vfp, d_sub), F(__muldf3vfp, d_mul),
    F(__eqsf2vfp, f_eq), F(__nesf2vfp, f_ne), F(__gtsf2vfp, f_gt), F(__gesf2vfp, f_ge),
    F(__ltsf2vfp, f_lt), F(__lesf2vfp, f_le), F(__gtdf2vfp, dd_gt), F(__gedf2vfp, dd_ge),
    F(__ltdf2vfp, dd_lt), F(__extendsfdf2vfp, f_ext), F(__truncdfsf2vfp, d_trunc),
    F(__fixsfsivfp, f_toi), F(__fixdfsivfp, d_toi), F(__fixsfdi, f_toll), F(__fixdfdi, d_toll),
    F(__fixunssfdi, f_toull), F(__floatsisfvfp, i_tof), F(__floatunssisfvfp, u_tof),
    F(__floatsidfvfp, i_tod), F(__floatdidf, ll_tod), F(__floatdisf, ll_tof),
    F(__divsi3, i_div), F(__modsi3, i_mod), F(__udivsi3, u_div), F(__umodsi3, u_mod),
    F(__divdi3, ll_div), F(__moddi3, ll_mod), F(__muldi3, ll_mul), F(__udivdi3, ull_div),
    F(__umoddi3, ull_mod), F(__cmpdi2, ll_cmp),
    // math softfp
    F(acosf, m_acosf), F(cosf, m_cosf), F(sinf, m_sinf), F(tanf, m_tanf), F(expf, m_expf),
    F(logf, m_logf), F(log10f, m_log10f), F(floorf, m_floorf), F(lrintf, m_lrintf), F(cos, m_cos),
    F(sin, m_sin), F(floor, m_floor), F(log10, m_log10), F(atan2, m_atan2), F(fmod, m_fmod),
    F(pow, m_pow), F(ldexp, m_ldexp),
};
const int darwin_imports_count = sizeof(darwin_imports) / sizeof(darwin_imports[0]);

void darwin_init(void) {
    d_stderrp = stderr;
    d_stdinp = stdin;
}
