// The audio thread must never wait: no memory allocation, no lock it can be held up on, no sleep, no file or socket. This file proves it
// (3.1.2, tools/tool-checks.sh rtcheck): it is linked into the core tests and the stress test, and while any thread is inside
// td::AudioEngine::process (wrapped at link time: -Wl,--wrap=<its mangled name>) every such call is caught here and reported with the place
// it came from. Nothing is changed about what the calls do: each one is handed on to the real function after the report.
//
// It is a home-made RealtimeSanitizer: Clang's -fsanitize=realtime does the same thing, but needs Clang 20, which the sandbox does not have.
// Linux only (glibc's __libc_* allocation entry points, dlsym (RTLD_NEXT) for the rest). On Windows the same Fermata code runs the same way; only
// the waking of other threads differs (juce::WaitableEvent::signal is SetEvent there, a mutex and a condition variable here), so that is allowed.
//
// RTGUARD_ABORT=1 stops at the first report (for a debugger); otherwise every report is printed once per place, and a summary at exit.
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

extern "C"
{
void* __libc_malloc (size_t);
void  __libc_free (void*);
void* __libc_calloc (size_t, size_t);
void* __libc_realloc (void*, size_t);
void* __libc_memalign (size_t, size_t);
void* __libc_valloc (size_t);
void* __libc_pvalloc (size_t);
}

namespace
{
thread_local int rtDepth = 0;        // > 0: this thread is inside AudioEngine::process
thread_local int allowDepth = 0;     // > 0: inside a call that is allowed here (waking another thread: see the wraps below)
thread_local int inHook = 0;         // > 0: inside the guard itself (its own report, dlsym): nothing is reported
std::atomic<int> reports { 0 }, places { 0 };
constexpr int kMaxPlaces = 256;
std::atomic<unsigned long> seen[kMaxPlaces];

struct RtDepth { RtDepth() { ++rtDepth; } ~RtDepth() { --rtDepth; } };
struct Allow { Allow() { ++allowDepth; } ~Allow() { --allowDepth; } };
struct Hook { Hook() { ++inHook; } ~Hook() { --inHook; } };

void writeAll (const char* s) { (void) ! write (2, s, strlen (s)); }

/** Prints the report once per place (the stack below the guard), then carries on (or aborts with RTGUARD_ABORT=1). */
void report (const char* what)
{
    if (inHook > 0 || rtDepth <= 0 || allowDepth > 0) return;
    Hook h;
    void* frames[24];
    const int n = backtrace (frames, 24);
    unsigned long hash = 1469598103934665603ul;
    for (int i = 2; i < n; ++i) { hash ^= (unsigned long) frames[i]; hash *= 1099511628211ul; }
    if (hash == 0) hash = 1;
    for (int i = 0; i < kMaxPlaces; ++i)
    {
        unsigned long expected = 0;
        if (seen[i].compare_exchange_strong (expected, hash)) { places.fetch_add (1); break; }
        if (expected == hash) { reports.fetch_add (1); return; }     // seen this place already
    }
    reports.fetch_add (1);
    char line[256];
    snprintf (line, sizeof (line), "\nREALTIME: %s called inside AudioEngine::process (report %d, place %d)\n", what, reports.load(), places.load());
    writeAll (line);
    backtrace_symbols_fd (frames + 2, n - 2, 2);
    writeAll ("REALTIME-END\n");
    if (const char* a = getenv ("RTGUARD_ABORT"); a != nullptr && a[0] == '1') abort();
}

struct Summary
{
    ~Summary()
    {
        char line[160];
        snprintf (line, sizeof (line), "REALTIME GUARD: %d report(s) from %d place(s)%s\n", reports.load(), places.load(), reports.load() == 0 ? " - the audio path never waited" : "");
        writeAll (line);
    }
} summary;

/** The real functions, looked up before main (and on the spot by a wrapper that is called earlier than that). dlsym may allocate: that goes to
    __libc_malloc through the hook flag. */
template <typename F> F lookUp (const char* name, F& slot)
{
    if (slot == nullptr) { Hook h; slot = reinterpret_cast<F> (dlsym (RTLD_NEXT, name)); }
    return slot;
}
#define REAL(name) (lookUp (#name, real_##name))
int (*real_pthread_mutex_lock) (pthread_mutex_t*) = nullptr;
int (*real_pthread_mutex_timedlock) (pthread_mutex_t*, const struct timespec*) = nullptr;
int (*real_pthread_rwlock_rdlock) (pthread_rwlock_t*) = nullptr;
int (*real_pthread_rwlock_wrlock) (pthread_rwlock_t*) = nullptr;
int (*real_pthread_cond_wait) (pthread_cond_t*, pthread_mutex_t*) = nullptr;
int (*real_pthread_cond_timedwait) (pthread_cond_t*, pthread_mutex_t*, const struct timespec*) = nullptr;
int (*real_pthread_join) (pthread_t, void**) = nullptr;
int (*real_pthread_create) (pthread_t*, const pthread_attr_t*, void* (*) (void*), void*) = nullptr;
int (*real_nanosleep) (const struct timespec*, struct timespec*) = nullptr;
int (*real_clock_nanosleep) (clockid_t, int, const struct timespec*, struct timespec*) = nullptr;
int (*real_usleep) (useconds_t) = nullptr;
unsigned int (*real_sleep) (unsigned int) = nullptr;
int (*real_sched_yield) () = nullptr;
ssize_t (*real_read) (int, void*, size_t) = nullptr;
ssize_t (*real_write) (int, const void*, size_t) = nullptr;
ssize_t (*real_pread) (int, void*, size_t, off_t) = nullptr;
ssize_t (*real_pwrite) (int, const void*, size_t, off_t) = nullptr;
ssize_t (*real_readv) (int, const struct iovec*, int) = nullptr;
ssize_t (*real_writev) (int, const struct iovec*, int) = nullptr;
int (*real_open) (const char*, int, ...) = nullptr;
int (*real_open64) (const char*, int, ...) = nullptr;
int (*real_openat) (int, const char*, int, ...) = nullptr;
int (*real_close) (int) = nullptr;
int (*real_fsync) (int) = nullptr;
int (*real_fdatasync) (int) = nullptr;
off_t (*real_lseek) (int, off_t, int) = nullptr;
FILE* (*real_fopen) (const char*, const char*) = nullptr;
FILE* (*real_fopen64) (const char*, const char*) = nullptr;
int (*real_fclose) (FILE*) = nullptr;
int (*real_stat) (const char*, struct stat*) = nullptr;
int (*real_lstat) (const char*, struct stat*) = nullptr;
int (*real_access) (const char*, int) = nullptr;
int (*real_unlink) (const char*) = nullptr;
int (*real_rename) (const char*, const char*) = nullptr;
int (*real_mkdir) (const char*, mode_t) = nullptr;
int (*real_poll) (struct pollfd*, nfds_t, int) = nullptr;
int (*real_select) (int, fd_set*, fd_set*, fd_set*, struct timeval*) = nullptr;
ssize_t (*real_send) (int, const void*, size_t, int) = nullptr;
ssize_t (*real_recv) (int, void*, size_t, int) = nullptr;
int (*real_connect) (int, const struct sockaddr*, socklen_t) = nullptr;

__attribute__ ((constructor)) void lookUpAll()
{
    REAL (pthread_mutex_lock); REAL (pthread_mutex_timedlock); REAL (pthread_rwlock_rdlock); REAL (pthread_rwlock_wrlock);
    REAL (pthread_cond_wait); REAL (pthread_cond_timedwait); REAL (pthread_join); REAL (pthread_create);
    REAL (nanosleep); REAL (clock_nanosleep); REAL (usleep); REAL (sleep); REAL (sched_yield);
    REAL (read); REAL (write); REAL (pread); REAL (pwrite); REAL (readv); REAL (writev);
    REAL (open); REAL (open64); REAL (openat); REAL (close); REAL (fsync); REAL (fdatasync); REAL (lseek);
    REAL (fopen); REAL (fopen64); REAL (fclose); REAL (stat); REAL (lstat); REAL (access); REAL (unlink); REAL (rename); REAL (mkdir);
    REAL (poll); REAL (select); REAL (send); REAL (recv); REAL (connect);
}

mode_t modeArg (int flags, va_list& ap) { return (flags & (O_CREAT | O_TMPFILE)) ? (mode_t) va_arg (ap, int) : 0; }
} // namespace

// ---- the wrapped Fermata / JUCE functions (the link line names each one with -Wl,--wrap=) ----
extern "C"
{
void __real__ZN2td11AudioEngine7processEPKPKfiPKPfii (void* self, const float* const* in, int numIn, float* const* out, int numOut, int n);
void __wrap__ZN2td11AudioEngine7processEPKPKfiPKPfii (void* self, const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    RtDepth d;
    __real__ZN2td11AudioEngine7processEPKPKfiPKPfii (self, in, numIn, out, numOut, n);
}
// juce::WaitableEvent::signal() const and juce::Thread::notify() const (which calls it): how the audio thread wakes the disk writer and the mixer
// helpers. SetEvent (no lock) on Windows, where the program runs; a mutex and a condition variable on Linux. Allowed through.
void __real__ZNK4juce13WaitableEvent6signalEv (const void* self);
void __wrap__ZNK4juce13WaitableEvent6signalEv (const void* self)
{
    Allow a;
    __real__ZNK4juce13WaitableEvent6signalEv (self);
}
void __real__ZNK4juce6Thread6notifyEv (const void* self);
void __wrap__ZNK4juce6Thread6notifyEv (const void* self)
{
    Allow a;
    __real__ZNK4juce6Thread6notifyEv (self);
}
}

// ---- memory ----
extern "C"
{
void* malloc (size_t n)                      { report ("malloc"); return __libc_malloc (n); }
void  free (void* p)                         { if (p != nullptr) report ("free"); __libc_free (p); }
void* calloc (size_t a, size_t b)            { report ("calloc"); return __libc_calloc (a, b); }
void* realloc (void* p, size_t n)            { report ("realloc"); return __libc_realloc (p, n); }
void* memalign (size_t al, size_t n)         { report ("memalign"); return __libc_memalign (al, n); }
void* aligned_alloc (size_t al, size_t n)    { report ("aligned_alloc"); return __libc_memalign (al, n); }
void* valloc (size_t n)                      { report ("valloc"); return __libc_valloc (n); }
void* pvalloc (size_t n)                     { report ("pvalloc"); return __libc_pvalloc (n); }
int posix_memalign (void** out, size_t al, size_t n)
{
    report ("posix_memalign");
    void* p = __libc_memalign (al, n);
    if (p == nullptr) return ENOMEM;
    *out = p; return 0;
}

// ---- locks, waits and sleeps ----
int pthread_mutex_lock (pthread_mutex_t* m)                                           { report ("pthread_mutex_lock"); return REAL (pthread_mutex_lock) (m); }
int pthread_mutex_timedlock (pthread_mutex_t* m, const struct timespec* t)             { report ("pthread_mutex_timedlock"); return REAL (pthread_mutex_timedlock) (m, t); }
int pthread_rwlock_rdlock (pthread_rwlock_t* l)                                        { report ("pthread_rwlock_rdlock"); return REAL (pthread_rwlock_rdlock) (l); }
int pthread_rwlock_wrlock (pthread_rwlock_t* l)                                        { report ("pthread_rwlock_wrlock"); return REAL (pthread_rwlock_wrlock) (l); }
int pthread_cond_wait (pthread_cond_t* c, pthread_mutex_t* m)                          { report ("pthread_cond_wait"); return REAL (pthread_cond_wait) (c, m); }
int pthread_cond_timedwait (pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) { report ("pthread_cond_timedwait"); return REAL (pthread_cond_timedwait) (c, m, t); }
int pthread_join (pthread_t t, void** r)                                               { report ("pthread_join"); return REAL (pthread_join) (t, r); }
int pthread_create (pthread_t* t, const pthread_attr_t* a, void* (*fn) (void*), void* arg) { report ("pthread_create"); return REAL (pthread_create) (t, a, fn, arg); }
int nanosleep (const struct timespec* req, struct timespec* rem)                       { report ("nanosleep"); return REAL (nanosleep) (req, rem); }
int clock_nanosleep (clockid_t c, int flags, const struct timespec* req, struct timespec* rem) { report ("clock_nanosleep"); return REAL (clock_nanosleep) (c, flags, req, rem); }
int usleep (useconds_t us)                                                             { report ("usleep"); return REAL (usleep) (us); }
unsigned int sleep (unsigned int s)                                                    { report ("sleep"); return REAL (sleep) (s); }
int sched_yield ()                                                                     { report ("sched_yield"); return REAL (sched_yield) (); }

// ---- files and sockets ----
ssize_t read (int fd, void* buf, size_t n)                                             { report ("read"); return REAL (read) (fd, buf, n); }
ssize_t write (int fd, const void* buf, size_t n)                                      { report ("write"); return REAL (write) (fd, buf, n); }
ssize_t pread (int fd, void* buf, size_t n, off_t o)                                   { report ("pread"); return REAL (pread) (fd, buf, n, o); }
ssize_t pwrite (int fd, const void* buf, size_t n, off_t o)                            { report ("pwrite"); return REAL (pwrite) (fd, buf, n, o); }
ssize_t readv (int fd, const struct iovec* v, int c)                                   { report ("readv"); return REAL (readv) (fd, v, c); }
ssize_t writev (int fd, const struct iovec* v, int c)                                  { report ("writev"); return REAL (writev) (fd, v, c); }
int open (const char* path, int flags, ...)
{
    va_list ap; va_start (ap, flags); const mode_t mode = modeArg (flags, ap); va_end (ap);
    report ("open"); return REAL (open) (path, flags, mode);
}
int open64 (const char* path, int flags, ...)
{
    va_list ap; va_start (ap, flags); const mode_t mode = modeArg (flags, ap); va_end (ap);
    report ("open64"); return REAL (open64) (path, flags, mode);
}
int openat (int dirfd, const char* path, int flags, ...)
{
    va_list ap; va_start (ap, flags); const mode_t mode = modeArg (flags, ap); va_end (ap);
    report ("openat"); return REAL (openat) (dirfd, path, flags, mode);
}
int close (int fd)                                                                     { report ("close"); return REAL (close) (fd); }
int fsync (int fd)                                                                     { report ("fsync"); return REAL (fsync) (fd); }
int fdatasync (int fd)                                                                 { report ("fdatasync"); return REAL (fdatasync) (fd); }
off_t lseek (int fd, off_t o, int w)                                                   { report ("lseek"); return REAL (lseek) (fd, o, w); }
FILE* fopen (const char* p, const char* m)                                             { report ("fopen"); return REAL (fopen) (p, m); }
FILE* fopen64 (const char* p, const char* m)                                           { report ("fopen64"); return REAL (fopen64) (p, m); }
int fclose (FILE* s)                                                                   { report ("fclose"); return REAL (fclose) (s); }
int stat (const char* p, struct stat* s)                                               { report ("stat"); return REAL (stat) (p, s); }
int lstat (const char* p, struct stat* s)                                              { report ("lstat"); return REAL (lstat) (p, s); }
int access (const char* p, int m)                                                      { report ("access"); return REAL (access) (p, m); }
int unlink (const char* p)                                                             { report ("unlink"); return REAL (unlink) (p); }
int rename (const char* a, const char* b)                                              { report ("rename"); return REAL (rename) (a, b); }
int mkdir (const char* p, mode_t m)                                                    { report ("mkdir"); return REAL (mkdir) (p, m); }
int poll (struct pollfd* fds, nfds_t n, int timeout)                                   { report ("poll"); return REAL (poll) (fds, n, timeout); }
int select (int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* t)                 { report ("select"); return REAL (select) (n, r, w, e, t); }
ssize_t send (int fd, const void* b, size_t n, int fl)                                 { report ("send"); return REAL (send) (fd, b, n, fl); }
ssize_t recv (int fd, void* b, size_t n, int fl)                                       { report ("recv"); return REAL (recv) (fd, b, n, fl); }
int connect (int fd, const struct sockaddr* a, socklen_t l)                            { report ("connect"); return REAL (connect) (fd, a, l); }
} // extern "C"

namespace td::rtguard
{
/** For the tests' own use: how many reports so far (0 = clean). */
int reportCount() { return reports.load(); }
}
