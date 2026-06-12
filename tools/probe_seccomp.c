#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>

static volatile sig_atomic_t got_sigsys = 0;
static volatile long sigsys_nr = -1;

static jmp_buf jmp;

void sigsys_handler(int sig, siginfo_t *info, void *ctx) {
    sigsys_nr = info->si_syscall;
    got_sigsys = 1;
    /* Return -ENOSYS from the syscall */
    #ifdef __aarch64__
    ucontext_t *uc = (ucontext_t *)ctx;
    uc->uc_mcontext.regs[0] = -38; /* -ENOSYS */
    #endif
}

int main() {
    /* Install SIGSYS handler to catch blocked syscalls */
    struct sigaction sa;
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);
    
    struct {
        long nr;
        const char *name;
    } probes[] = {
        {439, "faccessat2"},
        {333, "io_pgetevents"},
        {293, "rseq"},
        {291, "io_cancel"},
        {227, "io_setup"},
        {48,  "faccessat(old)"},
        {56,  "openat"},
        {63,  "read"},
        {64,  "write"},
        {57,  "close"},
        {222, "mmap"},
        {226, "mprotect"},
        {93,  "exit"},
        {94,  "exit_group"},
        {131, "tgkill"},
        {174, "getuid"},
        {175, "geteuid"},
        {176, "getgid"},
        {177, "getegid"},
        {204, "getpid"},
        {134, "rt_sigaction"},
        {135, "rt_sigprocmask"},
        {139, "sysinfo"},
        {168, "sched_yield"},
        {231, "clock_gettime"},
        {233, "clock_getres"},
        {240, "futex"},
        {160, "uname"},
        {169, "gettimeofday"},
        {102, "gettid"},
        {278, "getrandom"},
        {435, "io_uring_setup"},
        {436, "io_uring_enter"},
        {437, "io_uring_register"},
        {241, "mbind"},
        {235, "mremap"},
        {234, "msync"},
        {239, "madvise"},
        {163, "mlockall"},
        {165, "munlockall"},
        {208, "sched_setparam"},
        {209, "sched_setscheduler"},
        {210, "sched_getscheduler"},
        {211, "sched_getparam"},
        {212, "sched_setaffinity"},
        {213, "sched_getaffinity"},
        {214, "sched_yield(214)"},
        {215, "sched_get_priority_max"},
        {216, "sched_get_priority_min"},
        {217, "sched_rr_get_interval"},
    };
    
    int n = sizeof(probes) / sizeof(probes[0]);
    
    fprintf(stderr, "Probing %d syscalls:\n\n", n);
    
    for (int i = 0; i < n; i++) {
        got_sigsys = 0;
        sigsys_nr = -1;
        
        /* Try the syscall */
        long ret = syscall(probes[i].nr, 0, 0, 0, 0, 0, 0);
        
        if (got_sigsys) {
            fprintf(stderr, "  [%3ld] %-25s → SIGSYS (BLOCKED by seccomp, nr=%ld)\n",
                    probes[i].nr, probes[i].name, sigsys_nr);
        } else if (ret == -1) {
            fprintf(stderr, "  [%3ld] %-25s → ENOSYS (%s)\n",
                    probes[i].nr, probes[i].name, strerror(errno));
        } else {
            fprintf(stderr, "  [%3ld] %-25s → ret=%ld (allowed)\n",
                    probes[i].nr, probes[i].name, ret);
        }
    }
    
    fprintf(stderr, "\nDone.\n");
    return 0;
}
