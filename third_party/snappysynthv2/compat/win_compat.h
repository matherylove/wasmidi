#ifndef SNAPPY_WIN_COMPAT_H
#define SNAPPY_WIN_COMPAT_H

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else

#include <errno.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

typedef int BOOL;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef void *LPVOID;
typedef void *HINSTANCE;
typedef void *HMODULE;
typedef void *FARPROC;
typedef void *HWND;
typedef unsigned int UINT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef unsigned int MMRESULT;
typedef struct {
    char *lpData;
    DWORD dwBufferLength;
    DWORD dwBytesRecorded;
} MIDIHDR;
#ifndef MMSYSERR_NOERROR
#define MMSYSERR_NOERROR 0u
#define MMSYSERR_INVALPARAM 11u
#define MIDIERR_NOTREADY 67u
#endif

#define TRUE 1
#define FALSE 0
#define WINAPI
#define CALLBACK
#define __stdcall
#define EXPORT __attribute__((visibility("default")))
#define FORCEINLINE inline __attribute__((always_inline))

#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE NULL
#define CSIDL_APPDATA 0x001a
#define SUCCEEDED(hr) ((hr) >= 0)

#define _stricmp strcasecmp
#define _aligned_free free
static inline void *_aligned_malloc(size_t size, size_t align) {
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) return NULL;
    return p;
}

typedef union {
    struct { uint32_t LowPart; int32_t HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct {
    time_t tv_sec;
    long tv_nsec;
} FILETIME;

typedef struct {
    FILETIME ftLastWriteTime;
} WIN32_FIND_DATA;

typedef struct {
    uint16_t wYear;
    uint16_t wMonth;
    uint16_t wDayOfWeek;
    uint16_t wDay;
    uint16_t wHour;
    uint16_t wMinute;
    uint16_t wSecond;
    uint16_t wMilliseconds;
} SYSTEMTIME;

typedef struct {
    DWORD dwNumberOfProcessors;
} SYSTEM_INFO;

typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_init(cs, NULL); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(cs); }
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(cs); }

typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void AcquireSRWLockExclusive(SRWLOCK *lock) { pthread_rwlock_wrlock(lock); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *lock) { pthread_rwlock_unlock(lock); }

typedef int INIT_ONCE;
typedef INIT_ONCE *PINIT_ONCE;
typedef void *PVOID;
#define INIT_ONCE_STATIC_INIT 0
static inline BOOL InitOnceExecuteOnce(PINIT_ONCE once,
                                       BOOL (CALLBACK *fn)(PINIT_ONCE, PVOID, PVOID *),
                                       PVOID param,
                                       PVOID *context) {
    if (__sync_bool_compare_and_swap(once, 0, 1)) {
        BOOL ok = fn(once, param, context);
        __sync_lock_test_and_set(once, 2);
        return ok;
    }
    while (__sync_val_compare_and_swap(once, 1, 1) == 1) sched_yield();
    return TRUE;
}

typedef struct ss_handle {
    int type;
    union {
        struct {
#ifdef __EMSCRIPTEN__
            // Emscripten hot path: events are futex-backed atomics. This avoids
            // a pthread mutex + condition-variable lock/unlock pair for every
            // worker start/done signal in every synth render block.
            volatile uint32_t signaled;
            int manual_reset;
#else
            pthread_mutex_t mutex;
            pthread_cond_t cond;
            int manual_reset;
            int signaled;
#endif
        } event;
        pthread_mutex_t mutex;
        pthread_t thread;
    } u;
} *HANDLE;

enum { SS_HANDLE_EVENT = 1, SS_HANDLE_MUTEX = 2, SS_HANDLE_THREAD = 3 };

static inline HANDLE CreateEvent(void *attr, BOOL manual_reset, BOOL initial_state, const char *name) {
    (void)attr; (void)name;
    HANDLE h = (HANDLE)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->type = SS_HANDLE_EVENT;
#ifdef __EMSCRIPTEN__
    h->u.event.manual_reset = manual_reset ? 1 : 0;
    __atomic_store_n(&h->u.event.signaled,
                     initial_state ? 1u : 0u,
                     __ATOMIC_SEQ_CST);
#else
    pthread_mutex_init(&h->u.event.mutex, NULL);
    pthread_cond_init(&h->u.event.cond, NULL);
    h->u.event.manual_reset = manual_reset ? 1 : 0;
    h->u.event.signaled = initial_state ? 1 : 0;
#endif
    return h;
}

static inline HANDLE CreateMutex(void *attr, BOOL initial_owner, const char *name) {
    (void)attr; (void)name;
    HANDLE h = (HANDLE)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->type = SS_HANDLE_MUTEX;
    pthread_mutex_init(&h->u.mutex, NULL);
    if (initial_owner) pthread_mutex_lock(&h->u.mutex);
    return h;
}

static inline BOOL SetEvent(HANDLE h) {
    if (!h || h->type != SS_HANDLE_EVENT) return FALSE;
#ifdef __EMSCRIPTEN__
    const uint32_t was_signaled =
        __atomic_exchange_n(&h->u.event.signaled, 1u, __ATOMIC_SEQ_CST);
    // Win32 SetEvent on an already-signaled event is a no-op. Avoid redundant
    // futex broadcasts when every render worker re-signals the manual producer
    // barrier after waking.
    if (!was_signaled) {
        emscripten_futex_wake((void*)&h->u.event.signaled,
                              h->u.event.manual_reset ? INT_MAX : 1);
    }
#else
    pthread_mutex_lock(&h->u.event.mutex);
    h->u.event.signaled = 1;
    if (h->u.event.manual_reset) pthread_cond_broadcast(&h->u.event.cond);
    else pthread_cond_signal(&h->u.event.cond);
    pthread_mutex_unlock(&h->u.event.mutex);
#endif
    return TRUE;
}

static inline BOOL ResetEvent(HANDLE h) {
    if (!h || h->type != SS_HANDLE_EVENT) return FALSE;
#ifdef __EMSCRIPTEN__
    __atomic_store_n(&h->u.event.signaled, 0u, __ATOMIC_SEQ_CST);
#else
    pthread_mutex_lock(&h->u.event.mutex);
    h->u.event.signaled = 0;
    pthread_mutex_unlock(&h->u.event.mutex);
#endif
    return TRUE;
}

#ifdef __EMSCRIPTEN__
static inline int ss_event_try_acquire(HANDLE h) {
    if (h->u.event.manual_reset)
        return __atomic_load_n(&h->u.event.signaled, __ATOMIC_SEQ_CST) != 0u;

    uint32_t expected = 1u;
    return __atomic_compare_exchange_n(&h->u.event.signaled,
                                       &expected,
                                       0u,
                                       0,
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
#endif

static inline DWORD ss_wait_event(HANDLE h, DWORD ms) {
#ifdef __EMSCRIPTEN__
    if (ss_event_try_acquire(h))
        return WAIT_OBJECT_0;
    if (ms == 0)
        return WAIT_TIMEOUT;

    const double deadline =
        ms == INFINITE
            ? 0.0
            : emscripten_get_now() + (double)ms;

    for (;;) {
        if (ss_event_try_acquire(h))
            return WAIT_OBJECT_0;

        double timeout_ms = INFINITY;
        if (ms != INFINITE) {
            timeout_ms = deadline - emscripten_get_now();
            if (timeout_ms <= 0.0)
                return WAIT_TIMEOUT;
        }

        // Waiting on value 0 is safe even if SetEvent races between the check
        // and this call: futex_wait returns immediately when the value differs.
        emscripten_futex_wait((void*)&h->u.event.signaled,
                              0u,
                              timeout_ms);
    }
#else
    DWORD rc = WAIT_OBJECT_0;
    pthread_mutex_lock(&h->u.event.mutex);
    if (!h->u.event.signaled) {
        if (ms == 0) {
            rc = WAIT_TIMEOUT;
        } else if (ms == INFINITE) {
            while (!h->u.event.signaled) pthread_cond_wait(&h->u.event.cond, &h->u.event.mutex);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000u;
            ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            while (!h->u.event.signaled) {
                if (pthread_cond_timedwait(&h->u.event.cond, &h->u.event.mutex, &ts) == ETIMEDOUT) {
                    rc = WAIT_TIMEOUT;
                    break;
                }
            }
        }
    }
    if (rc == WAIT_OBJECT_0 && !h->u.event.manual_reset) h->u.event.signaled = 0;
    pthread_mutex_unlock(&h->u.event.mutex);
    return rc;
#endif
}

static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!h) return WAIT_FAILED;
    if (h->type == SS_HANDLE_EVENT) return ss_wait_event(h, ms);
    if (h->type == SS_HANDLE_MUTEX) {
        if (ms == 0) return pthread_mutex_trylock(&h->u.mutex) == 0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
        pthread_mutex_lock(&h->u.mutex);
        return WAIT_OBJECT_0;
    }
    if (h->type == SS_HANDLE_THREAD) {
        pthread_join(h->u.thread, NULL);
        return WAIT_OBJECT_0;
    }
    return WAIT_FAILED;
}

static inline DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL wait_all, DWORD ms) {
    (void)wait_all;
    for (DWORD i = 0; i < count; ++i) {
        DWORD rc = WaitForSingleObject(handles[i], ms);
        if (rc != WAIT_OBJECT_0) return rc;
    }
    return WAIT_OBJECT_0;
}

static inline BOOL ReleaseMutex(HANDLE h) {
    if (!h || h->type != SS_HANDLE_MUTEX) return FALSE;
    return pthread_mutex_unlock(&h->u.mutex) == 0;
}

static inline BOOL CloseHandle(HANDLE h) {
    if (!h) return FALSE;
    if (h->type == SS_HANDLE_EVENT) {
#ifndef __EMSCRIPTEN__
        pthread_mutex_destroy(&h->u.event.mutex);
        pthread_cond_destroy(&h->u.event.cond);
#endif
    } else if (h->type == SS_HANDLE_MUTEX) {
        pthread_mutex_destroy(&h->u.mutex);
    }
    free(h);
    return TRUE;
}

typedef struct {
    void *(*fn)(void *);
    void *arg;
} ss_thread_start;

static inline void *ss_thread_trampoline(void *arg) {
    ss_thread_start *s = (ss_thread_start *)arg;
    void *(*fn)(void *) = s->fn;
    void *fn_arg = s->arg;
    free(s);
    return fn(fn_arg);
}

static inline HANDLE CreateThread(void *sa, size_t stack, DWORD (WINAPI *fn)(LPVOID), void *arg, DWORD flags, DWORD *tid) {
    (void)sa; (void)stack; (void)flags; (void)tid;
    HANDLE h = (HANDLE)calloc(1, sizeof(*h));
    ss_thread_start *s = (ss_thread_start *)malloc(sizeof(*s));
    if (!h || !s) { free(h); free(s); return NULL; }
    h->type = SS_HANDLE_THREAD;
    s->fn = (void *(*)(void *))fn;
    s->arg = arg;
    if (pthread_create(&h->u.thread, NULL, ss_thread_trampoline, s) != 0) {
        free(s); free(h); return NULL;
    }
    return h;
}

static inline uintptr_t _beginthreadex(void *sa, unsigned stack, unsigned (__stdcall *fn)(void *), void *arg, unsigned flags, unsigned *tid) {
    return (uintptr_t)CreateThread(sa, stack, (DWORD (WINAPI *)(LPVOID))fn, arg, flags, (DWORD *)tid);
}

static inline LONG InterlockedIncrement(volatile LONG *v) { return __sync_add_and_fetch(v, 1); }
static inline LONG InterlockedDecrement(volatile LONG *v) { return __sync_sub_and_fetch(v, 1); }
static inline LONG InterlockedExchange(volatile LONG *v, LONG x) { return __sync_lock_test_and_set(v, x); }
static inline LONG InterlockedAdd(volatile LONG *v, LONG x) { return __sync_add_and_fetch(v, x); }
static inline LONG InterlockedCompareExchange(volatile LONG *v, LONG x, LONG cmp) {
    return __sync_val_compare_and_swap(v, cmp, x);
}
static inline LONG InterlockedOr(volatile LONG *v, LONG x) { return __sync_fetch_and_or(v, x); }

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *out) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *out) {
    out->QuadPart = 1000000000LL;
    return TRUE;
}

static inline void Sleep(DWORD ms) {
    if (ms == 0) sched_yield();
    else usleep((useconds_t)ms * 1000u);
}

static inline void GetLocalTime(SYSTEMTIME *st) {
    struct timespec ts;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    st->wYear = (uint16_t)(tmv.tm_year + 1900);
    st->wMonth = (uint16_t)(tmv.tm_mon + 1);
    st->wDayOfWeek = (uint16_t)tmv.tm_wday;
    st->wDay = (uint16_t)tmv.tm_mday;
    st->wHour = (uint16_t)tmv.tm_hour;
    st->wMinute = (uint16_t)tmv.tm_min;
    st->wSecond = (uint16_t)tmv.tm_sec;
    st->wMilliseconds = (uint16_t)(ts.tv_nsec / 1000000L);
}

static inline DWORD GetTickCount(void) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (DWORD)(qpc.QuadPart / 1000000LL);
}

static inline void GetSystemInfo(SYSTEM_INFO *si) {
#ifdef __EMSCRIPTEN__
    int n = emscripten_num_logical_cores();
    si->dwNumberOfProcessors = (DWORD)(n > 0 ? n : 1);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = (DWORD)(n > 0 ? n : 1);
#endif
}

static inline HANDLE FindFirstFile(const char *path, WIN32_FIND_DATA *data) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return INVALID_HANDLE_VALUE;
    if (data) {
        data->ftLastWriteTime.tv_sec = st.st_mtime;
        data->ftLastWriteTime.tv_nsec = 0;
    }
    return (HANDLE)1;
}

static inline BOOL FindClose(HANDLE h) { (void)h; return TRUE; }

static inline int CompareFileTime(const FILETIME *a, const FILETIME *b) {
    if (a->tv_sec != b->tv_sec) return (a->tv_sec > b->tv_sec) ? 1 : -1;
    if (a->tv_nsec != b->tv_nsec) return (a->tv_nsec > b->tv_nsec) ? 1 : -1;
    return 0;
}

static inline int SHGetFolderPathA(void *hwnd, int csidl, void *token, DWORD flags, char *out) {
    (void)hwnd; (void)csidl; (void)token; (void)flags;
    const char *base = getenv("XDG_CONFIG_HOME");
    if (!base || !base[0]) {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return -1;
        snprintf(out, MAX_PATH, "%s/.config", home);
    } else {
        snprintf(out, MAX_PATH, "%s", base);
    }
    return 0;
}

static inline DWORD GetModuleFileNameA(HMODULE mod, char *buf, DWORD len) {
    (void)mod;
    if (!buf || len == 0) return 0;
    if (!getcwd(buf, (size_t)len)) return 0;
    return (DWORD)strlen(buf);
}

static inline int MessageBoxA(HWND hwnd, const char *text, const char *caption, UINT type) {
    (void)hwnd; (void)type;
    fprintf(stderr, "%s: %s\n", caption ? caption : "SnappySynth", text ? text : "");
    return 0;
}

#define MB_ICONERROR 0
#define MB_OK 0
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_ABOVE_NORMAL 1
static inline BOOL SetThreadPriority(HANDLE h, int priority) { (void)h; (void)priority; return TRUE; }

#define _fseeki64 fseeko
#define _ftelli64 ftello

#endif

#endif
