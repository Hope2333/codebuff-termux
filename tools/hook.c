/**
 * hook.c — glibc LD_PRELOAD hook for running Codebuff on Android without proot
 *
 * Compile against glibc (not Bionic) so glibc's ld.so can load it:
 *   gcc -fPIC -shared -o hook.so hook.c \
 *     -I/data/data/com.termux/files/usr/glibc/include \
 *     -L/data/data/com.termux/files/usr/glibc/lib \
 *     -nostdlib -lc -ldl
 *
 * Hooks:
 *   - File access (open/openat/fopen/fopen64) → redirect /proc/stat,
 *     /proc/cpuinfo, /sys/devices/system/cpu/*, /proc/loadavg
 *   - sysconf() → return fake CPU count (avoid getcpu syscall)
 *   - sched_getaffinity() → return fake CPU mask (avoid blocked syscall)
 *   - uname() → fake node name if needed
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sched.h>
#include <sys/utsname.h>
#include <errno.h>
#include <signal.h>
#include <ucontext.h>

/* ── Glue for -nostdlib ── */
int close(int fd);
ssize_t write(int fd, const void *buf, size_t count);
int __libc_current_sigrtmin(void);
long syscall(long number, ...);
int *__errno_location(void);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

/* ── Fake file content directory ── */
#define TMPDIR "/data/data/com.termux/files/usr/tmp"

/* Lazily-created fake file paths */
static int fake_files_created = 0;

/* Thread-safe rotating buffer (benign race on counter — path writes are short) */
static char fake_buf[8][256];
static volatile int fake_buf_idx = 0;

static const char *get_fake_path(const char *original) {
    int idx = fake_buf_idx++ & 7;
    char *buf = fake_buf[idx];

    /* /proc/stat → /usr/tmp/.fb-proc-stat */
    if (strcmp(original, "/proc/stat") == 0) {
        snprintf(buf, sizeof(buf), TMPDIR "/.fb-proc-stat");
        return buf;
    }

    /* /proc/cpuinfo → /usr/tmp/.fb-cpuinfo */
    if (strcmp(original, "/proc/cpuinfo") == 0) {
        snprintf(buf, sizeof(buf), TMPDIR "/.fb-cpuinfo");
        return buf;
    }

    /* /proc/loadavg → /usr/tmp/.fb-loadavg */
    if (strcmp(original, "/proc/loadavg") == 0) {
        snprintf(buf, sizeof(buf), TMPDIR "/.fb-loadavg");
        return buf;
    }

    /* /proc/self/status → /usr/tmp/.fb-self-status */
    if (strcmp(original, "/proc/self/status") == 0 ||
        strcmp(original, "/proc/self/stat") == 0) {
        snprintf(buf, sizeof(buf), TMPDIR "/.fb-self-status");
        return buf;
    }

    /* /sys/devices/system/cpu/present → /usr/tmp/.fb-cpu-present */
    if (strcmp(original, "/sys/devices/system/cpu/present") == 0 ||
        strstr(original, "/sys/devices/system/cpu") == original) {
        /* Only redirect known paths, not deeper cpuN/cpufreq etc. */
        if (strcmp(original, "/sys/devices/system/cpu/present") == 0) {
            snprintf(buf, sizeof(buf), TMPDIR "/.fb-cpu-present");
            return buf;
        }
        if (strcmp(original, "/sys/devices/system/cpu/online") == 0) {
            snprintf(buf, sizeof(buf), TMPDIR "/.fb-cpu-online");
            return buf;
        }
        if (strcmp(original, "/sys/devices/system/cpu") == 0) {
            snprintf(buf, sizeof(buf), TMPDIR "/.fb-cpu-dir");
            return buf;
        }
    }

    /* Also catch /sys/devices/system/cpu (no trailing slash) variants */
    if (strcmp(original, "/sys/devices/system/cpu") == 0) {
        snprintf(buf, sizeof(buf), TMPDIR "/.fb-cpu-dir");
        return buf;
    }

    return NULL; /* no redirect */
}

static void ensure_fake_files(void) {
    if (fake_files_created) return;
    fake_files_created = 1;

    /* Create tmp dir if needed */
    mkdir(TMPDIR, 0755);

    /* ── /proc/stat ── */
    FILE *f = fopen(TMPDIR "/.fb-proc-stat", "w");
    if (f) {
        fprintf(f,
            "cpu  0 0 0 0 0 0 0 0 0 0\n"
            "cpu0 0 0 0 0 0 0 0 0 0 0\n"
            "cpu1 0 0 0 0 0 0 0 0 0 0\n"
            "cpu2 0 0 0 0 0 0 0 0 0 0\n"
            "cpu3 0 0 0 0 0 0 0 0 0 0\n"
            "cpu4 0 0 0 0 0 0 0 0 0 0\n"
            "cpu5 0 0 0 0 0 0 0 0 0 0\n"
            "cpu6 0 0 0 0 0 0 0 0 0 0\n"
            "cpu7 0 0 0 0 0 0 0 0 0 0\n"
            "intr 0 0 0 0 0 0 0 0 0 0\n"
            "ctxt 0\n"
            "btime 0\n"
            "processes 0\n"
            "procs_running 1\n"
            "procs_blocked 0\n"
            "softirq 0 0 0 0 0 0 0 0 0 0\n");
        fclose(f);
        chmod(TMPDIR "/.fb-proc-stat", 0644);
    }

    /* ── /proc/cpuinfo (8-core ARM) ── */
    f = fopen(TMPDIR "/.fb-cpuinfo", "w");
    if (f) {
        for (int i = 0; i < 8; i++) {
            fprintf(f,
                "processor\t: %d\n"
                "BogoMIPS\t: 100.00\n"
                "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
                "CPU implementer\t: 0x41\n"
                "CPU architecture\t: 8\n"
                "CPU variant\t: 0x0\n"
                "CPU part\t: 0xd0d\n"
                "CPU revision\t: 2\n\n", i);
        }
        fclose(f);
        chmod(TMPDIR "/.fb-cpuinfo", 0644);
    }

    /* ── /proc/loadavg ── */
    f = fopen(TMPDIR "/.fb-loadavg", "w");
    if (f) {
        fprintf(f, "0.00 0.00 0.00 1/1 1\n");
        fclose(f);
        chmod(TMPDIR "/.fb-loadavg", 0644);
    }

    /* ── /sys/devices/system/cpu/present ── */
    f = fopen(TMPDIR "/.fb-cpu-present", "w");
    if (f) {
        fprintf(f, "0-7\n");
        fclose(f);
        chmod(TMPDIR "/.fb-cpu-present", 0644);
    }

    /* ── /sys/devices/system/cpu/online ── */
    f = fopen(TMPDIR "/.fb-cpu-online", "w");
    if (f) {
        fprintf(f, "0-7\n");
        fclose(f);
        chmod(TMPDIR "/.fb-cpu-online", 0644);
    }

    /* ── /proc/self/status (minimal) ── */
    f = fopen(TMPDIR "/.fb-self-status", "w");
    if (f) {
        fprintf(f,
            "Name:\tcodebuff\n"
            "State:\tR (running)\n"
            "Tgid:\t1\n"
            "Pid:\t1\n"
            "PPid:\t0\n"
            "TracerPid:\t0\n"
            "Uid:\t0\t0\t0\t0\n"
            "Gid:\t0\t0\t0\t0\n"
            "FDSize:\t256\n"
            "Threads:\t4\n"
            "Seccomp:\t0\n"
            "Cpus_allowed:\tff\n"
            "Cpus_allowed_list:\t0-7\n"
            "Mems_allowed:\t1\n"
            "Mems_allowed_list:\t0\n");
        fclose(f);
        chmod(TMPDIR "/.fb-self-status", 0644);
    }
}

/* ═══════════════════════════════════════════════
   File-access hooks (redirect /proc and /sys)
   ═══════════════════════════════════════════════ */

/* ── open() ── */
int open(const char *path, int flags, ...) {
    static int (*real_open)(const char *, int, ...) = NULL;
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    ensure_fake_files();
    const char *fake = get_fake_path(path);
    if (fake) path = fake;
    return real_open(path, flags, mode);
}

/* ── openat() ── */
int openat(int dirfd, const char *path, int flags, ...) {
    static int (*real_openat)(int, const char *, int, ...) = NULL;
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (path) {
        ensure_fake_files();
        const char *fake = get_fake_path(path);
        if (fake) path = fake;
    }
    return real_openat(dirfd, path, flags, mode);
}

/* ── openat64() — Bun imports this, not openat ── */
int openat64(int dirfd, const char *path, int flags, ...) {
    static int (*real_openat64)(int, const char *, int, ...) = NULL;
    if (!real_openat64) real_openat64 = dlsym(RTLD_NEXT, "openat64");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (path) {
        ensure_fake_files();
        const char *fake = get_fake_path(path);
        if (fake) path = fake;
    }
    return real_openat64(dirfd, path, flags, mode);
}

/* ── open64() ── */
int open64(const char *path, int flags, ...) {
    static int (*real_open64)(const char *, int, ...) = NULL;
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    ensure_fake_files();
    const char *fake = get_fake_path(path);
    if (fake) path = fake;
    return real_open64(path, flags, mode);
}

/* ── fopen() ── */
FILE *fopen(const char *path, const char *mode) {
    static FILE *(*real_fopen)(const char *, const char *) = NULL;
    if (!real_fopen) real_fopen = dlsym(RTLD_NEXT, "fopen");
    if (path) {
        ensure_fake_files();
        const char *fake = get_fake_path(path);
        if (fake) path = fake;
    }
    return real_fopen(path, mode);
}

/* ── fopen64() ── */
FILE *fopen64(const char *path, const char *mode) {
    static FILE *(*real_fopen64)(const char *, const char *) = NULL;
    if (!real_fopen64) real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
    if (path) {
        ensure_fake_files();
        const char *fake = get_fake_path(path);
        if (fake) path = fake;
    }
    return real_fopen64(path, mode);
}

/* ═══════════════════════════════════════════════
   libc function overrides to prevent blocked syscalls
   ═══════════════════════════════════════════════ */

/* ── sysconf() — avoid getcpu() syscall ── */
long sysconf(int name) {
    static long (*real_sysconf)(int) = NULL;
    if (!real_sysconf) real_sysconf = dlsym(RTLD_NEXT, "sysconf");

    switch (name) {
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        return 8;
    case _SC_PHYS_PAGES:
        return 4194304; /* 16 GB in pages */
    case _SC_AVPHYS_PAGES:
        return 2097152;
    default:
        return real_sysconf(name);
    }
}

/* ── sched_getaffinity() — avoid seccomp-blocked syscall ── */
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
    /* Return a fake mask: CPUs 0-7 are available */
    if (mask && cpusetsize >= sizeof(cpu_set_t)) {
        CPU_ZERO(mask);
        for (int i = 0; i < 8; i++) CPU_SET(i, mask);
    }
    /* Pretend success with 8 CPUs */
    return 8 * sizeof(unsigned long);
}

/* ── sched_setaffinity() — no-op ── */
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    (void)pid; (void)cpusetsize; (void)mask;
    return 0;
}

/* ── uname() — ensure node name is valid ── */
int uname(struct utsname *buf) {
    static int (*real_uname)(struct utsname *) = NULL;
    if (!real_uname) real_uname = dlsym(RTLD_NEXT, "uname");
    int ret = real_uname(buf);
    if (ret == 0 && buf->nodename[0] == '\0') {
        strcpy(buf->nodename, "android");
    }
    return ret;
}

/* ── Async-signal-safe integer to stderr ── */
static void write_uint(int fd, unsigned long val);

/* ═══════════════════════════════════════════════════════════════
   Process creation hooks — detect children that get killed
   by seccomp (SECCOMP_RET_KILL). systeminformation/cpu.js
   appears to spawn child processes that get killed.

   We intercept fork/clone/execve to log what children are doing.
   ═══════════════════════════════════════════════════════════════ */

/* ── fork() — log child process creation ── */
pid_t fork(void) {
    static pid_t (*real_fork)(void) = NULL;
    if (!real_fork) real_fork = dlsym(RTLD_NEXT, "fork");
    pid_t pid = real_fork();
    if (pid == 0) {
        write(2, "[hook.so] CHILD forked\n", 23);
    } else if (pid > 0) {
        write(2, "[hook.so] PARENT forked child ", 30);
        write_uint(2, (unsigned long)pid);
    }
    return pid;
}

/* ── Helper: copy envp array filtering out LD_PRELOAD ── */
static char **filter_envp(char *const envp[]) {
    if (!envp) return NULL;
    int count = 0;
    while (envp[count]) count++;
    char **new_env = malloc((count + 1) * sizeof(char *));
    if (!new_env) return (char **)envp;
    int j = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0)
            continue; /* skip — prevents bionic children from crashing */
        new_env[j++] = envp[i];
    }
    new_env[j] = NULL;
    return new_env;
}

/* ── execve() — strip LD_PRELOAD for child processes ── */
int execve(const char *pathname, char *const argv[], char *const envp[]) {
    static int (*real_execve)(const char *, char *const *, char *const *) = NULL;
    if (!real_execve) real_execve = dlsym(RTLD_NEXT, "execve");
    char **clean_env = filter_envp(envp);
    return real_execve(pathname, argv, (char *const *)clean_env);
}

/* ── execvp() — strip LD_PRELOAD from environ before exec ── */
int execvp(const char *file, char *const argv[]) {
    static int (*real_execvp)(const char *, char *const *) = NULL;
    if (!real_execvp) real_execvp = dlsym(RTLD_NEXT, "execvp");
    /* Temporarily unset LD_PRELOAD to prevent bionic children from
     * inheriting our glibc hook.so */
    char *old_preload = getenv("LD_PRELOAD");
    if (old_preload) {
        char *saved = strdup(old_preload);
        unsetenv("LD_PRELOAD");
        int ret = real_execvp(file, argv);
        setenv("LD_PRELOAD", saved, 1);
        free(saved);
        return ret;
    }
    return real_execvp(file, argv);
}

/* ═══════════════════════════════════════════════
   Syscall bypass — glibc 2.33+ uses faccessat2 (#439)
   which seccomp blocks. Override faccessat to use
   the older faccessat (#48) directly.
   ═══════════════════════════════════════════════ */

/* SYS_faccessat = 48 on ARM64, SYS_faccessat2 = 439 */
#ifndef SYS_faccessat
#define SYS_faccessat 48
#endif
#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif

/* ── faccessat() — bypass seccomp-blocked faccessat2 ── */
int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    /* For our fake /proc/* paths, always return success (accessible) */
    if (pathname) {
        if (strcmp(pathname, "/proc/stat") == 0 ||
            strcmp(pathname, "/proc/cpuinfo") == 0 ||
            strcmp(pathname, "/proc/loadavg") == 0 ||
            strcmp(pathname, "/proc/self/status") == 0 ||
            strcmp(pathname, "/proc/self/stat") == 0 ||
            strcmp(pathname, "/sys/devices/system/cpu/present") == 0 ||
            strcmp(pathname, "/sys/devices/system/cpu/online") == 0) {
            return 0; /* accessible */
        }
    }
    /* Use the older faccessat syscall directly, bypassing glibc's
       faccessat2-first implementation */
    return syscall(SYS_faccessat, dirfd, pathname, mode, flags);
}

/* ── access() — convenience wrapper that calls our faccessat ── */
int access(const char *pathname, int mode) {
    return faccessat(AT_FDCWD, pathname, mode, 0);
}

/* ═══════════════════════════════════════════════
   SIGSYS handler — catch seccomp-blocked syscalls
   instead of letting them kill the process.
   Android's seccomp blocks rseq, io_pgetevents, faccessat2, etc.
   ═══════════════════════════════════════════════ */

static volatile int sigsys_initialized = 0;

/* ── Async-signal-safe integer to stderr ── */
static void write_uint(int fd, unsigned long val) {
    char buf[32];
    char *p = buf + sizeof(buf) - 1;
    *p = '\n';
    if (val == 0) *--p = '0';
    else while (val) { *--p = '0' + (val % 10); val /= 10; }
    write(fd, p, buf + sizeof(buf) - 1 - p);
}

/* ── Set ucontext to make blocked syscall return -ENOSYS ── */
static void sigsys_return_enosys(void *ctx) {
#ifdef __aarch64__
    ucontext_t *uc = (ucontext_t *)ctx;
    /* ARM64: x0 = return value */
    uc->uc_mcontext.regs[0] = -38;  /* -ENOSYS */
#endif
}

static void sigsys_handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig;
    int nr = info->si_syscall;

    /* Log ALL blocked syscalls — critical for debugging */
    {
        static const char msg[] = "[hook.so] SIGSYS nr=";
        write(2, msg, sizeof(msg) - 1);
        write_uint(2, (unsigned long)nr);
        static const char msg2[] = " code=";
        write(2, msg2, sizeof(msg2) - 1);
        write_uint(2, (unsigned long)info->si_code);
        static const char msg3[] = " addr=";
        write(2, msg3, sizeof(msg3) - 1);
        write_uint(2, (unsigned long)(unsigned long)info->si_call_addr);
        write(2, "\n", 1);
    }

    /* For expected blocked syscalls, silently return -ENOSYS */
    if (nr == 293 || nr == 333 || nr == 439) {
        sigsys_return_enosys(ctx);
        return;
    }

    /* Unknown blocked syscall — still return ENOSYS to prevent crash */
    sigsys_return_enosys(ctx);
}

/* ── Write message to stderr (minimal, for debugging) ── */
static void debug_log(const char *msg) {
    static const char prefix[] = "[hook.so] ";
    write(2, prefix, 10);
    write(2, msg, strlen(msg));
    write(2, "\n", 1);
}

/* ═══════════════════════════════════════════════════════════════
   SIGACTION OVERRIDE — prevent anyone from replacing our SIGSYS handler
   
   Glibc or Bun might install their own SIGSYS handler late, which
   would override ours and cause "Bad system call" crashes.
   We intercept sigaction() and silently ignore requests that would
   replace our SIGSYS handler.
   
   KEY INSIGHT: Bun installs its own SIGSYS handler via direct
   rt_sigaction() syscall, bypassing our override. To ensure our
   handler takes precedence, a background thread re-installs it
   after Bun has started.
   ═══════════════════════════════════════════════════════════════ */

#include <pthread.h>

/* ── sigaction() — protect SIGSYS handler from being replaced ── */
static volatile int sigsys_installing = 0;

int sigaction(int signum, const struct sigaction *act,
              struct sigaction *oldact) {
    static int (*real_sigaction)(int, const struct sigaction *,
                                 struct sigaction *) = NULL;
    if (!real_sigaction)
        real_sigaction = dlsym(RTLD_NEXT, "sigaction");

    /* Allow our own install_sigsys_handler() to install the handler */
    if (signum == SIGSYS && act && !sigsys_installing) {
        /* Save the old/handler info if requested */
        if (oldact)
            return real_sigaction(signum, NULL, oldact);
        /* Silently ignore — our handler stays in place */
        return 0;
    }

    return real_sigaction(signum, act, oldact);
}

/* ── Install SIGSYS handler ── */
static void install_sigsys_handler(void) {
    if (sigsys_initialized) return;
    sigsys_initialized = 1;
    
    struct sigaction sa;
    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigsys_installing = 1;
    sigaction(SIGSYS, &sa, NULL);
    sigsys_installing = 0;
    debug_log("SIGSYS handler installed and locked");
}

/* ── Late SIGSYS handler installer (uses direct rt_sigaction syscall) ── */
static void *late_sigsys_installer(void *arg) {
    (void)arg;
    
    /* Build struct sigaction as raw bytes for rt_sigaction syscall.
     * Layout on ARM64 (32 bytes):
     *   [0-7]   handler pointer (sa_sigaction)
     *   [8-15]  sa_flags (SA_SIGINFO | SA_NODEFER)
     *   [16-23] sa_restorer (NULL)
     *   [24-31] sa_mask (empty)
     */
    unsigned char sa_buf[32];
    __builtin_memset(sa_buf, 0, sizeof(sa_buf));
    
    /* Handler pointer at offset 0 */
    void (*handler)(int, siginfo_t *, void *) = sigsys_handler;
    __builtin_memcpy(&sa_buf[0], &handler, 8);
    
    /* SA_SIGINFO = 4, SA_NODEFER = 0x80000000 on ARM64 */
    unsigned long flags = 4UL | 0x80000000UL;
    __builtin_memcpy(&sa_buf[8], &flags, 8);
    
    /* Re-install every 100ms for 8 seconds */
    for (int i = 0; i < 80; i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&ts, NULL);
        /* rt_sigaction syscall (NR=134 on ARM64) */
        syscall(134, SIGSYS, sa_buf, NULL, 8);
    }
    debug_log("SIGSYS handler late-install complete");
    return NULL;
}

/* ── Constructor: runs when the .so is loaded ── */
__attribute__((constructor))
static void hook_init(void) {
    install_sigsys_handler();
    /* Start late-installer thread to re-install after Bun */
    pthread_t tid;
    pthread_create(&tid, NULL, late_sigsys_installer, NULL);
    pthread_detach(tid);
}
