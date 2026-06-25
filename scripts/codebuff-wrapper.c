/**
 * codebuff-wrapper.c — Codebuff for Termux launcher (proot-only approach)
 *
 * Uses proot to bind-mount fake CPU/proc files, avoiding LD_PRELOAD
 * which doesn't work with Bun's direct syscalls.
 *
 * Build:
 *   gcc -O2 -s -o codebuff-wrapper codebuff-wrapper.c \
 *     -DCODEBUFF_BINARY='"/path/to/codebuff"' \
 *     -DPROOT_PATH='"/usr/bin/proot"' \
 *     -DFAKE_DIR='"/data/data/com.termux/files/usr/tmp/.codebuff-fake"'
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>

#ifndef CODEBUFF_BINARY
#  define CODEBUFF_BINARY \
    "/data/data/com.termux/files/usr/lib/codebuff/runtime/codebuff"
#endif
#ifndef PROOT_PATH
#  define PROOT_PATH \
    "/data/data/com.termux/files/usr/bin/proot"
#endif
#ifndef FAKE_DIR
#  define FAKE_DIR \
    "/data/data/com.termux/files/usr/tmp/.codebuff-fake"
#endif

static void ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == -1) mkdir(dir, 0755);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    chmod(path, 0644);
}

static void create_fake_files(void) {
    ensure_dir(FAKE_DIR);

    /* /proc/stat — 8 CPU cores */
    write_file(FAKE_DIR "/stat",
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
        "ctxt 0\nbtime 0\nprocesses 0\n"
        "procs_running 1\nprocs_blocked 0\n"
        "softirq 0 0 0 0 0 0 0 0 0 0\n");

    /* /proc/cpuinfo — 8 ARM cores */
    {
        FILE *f = fopen(FAKE_DIR "/cpuinfo", "w");
        if (f) {
            for (int i = 0; i < 8; i++)
                fprintf(f,
                    "processor\t: %d\n"
                    "BogoMIPS\t: 100.00\n"
                    "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
                    "CPU implementer\t: 0x41\n"
                    "CPU architecture\t: 8\n"
                    "CPU variant\t: 0x0\n"
                    "CPU part\t: 0xd0d\n"
                    "CPU revision\t: 2\n\n", i);
            fclose(f);
            chmod(FAKE_DIR "/cpuinfo", 0644);
        }
    }

    /* /sys/devices/system/cpu/present */
    write_file(FAKE_DIR "/cpu-present", "0-7\n");
    /* /sys/devices/system/cpu/online */
    write_file(FAKE_DIR "/cpu-online", "0-7\n");
    /* /proc/loadavg */
    write_file(FAKE_DIR "/loadavg", "0.00 0.00 0.00 1/1 1\n");
}

/* ── argv builder ── */
typedef struct { char **argv; int cap, len; } Argv;

static Argv *argv_new(int hint) {
    Argv *a = malloc(sizeof(Argv));
    a->cap = hint ? hint : 32;
    a->len = 0;
    a->argv = malloc(a->cap * sizeof(char *));
    return a;
}

static void argv_add(Argv *a, const char *s) {
    if (a->len >= a->cap - 1) {
        a->cap *= 2;
        a->argv = realloc(a->argv, a->cap * sizeof(char *));
    }
    a->argv[a->len++] = strdup(s);
}

static void argv_emit(Argv *a) { a->argv[a->len] = NULL; }

static void xunsetenv(const char *name) {
    (void)unsetenv(name);
}

int main(int argc, char *argv[]) {
    /* Strip LD_* env vars that could poison glibc loading */
    xunsetenv("LD_PRELOAD");
    xunsetenv("LD_LIBRARY_PATH");
    xunsetenv("LD_DEBUG");

    /* Check proot availability */
    struct stat st;
    int has_proot = (stat(PROOT_PATH, &st) == 0 && (st.st_mode & S_IXUSR));

    if (!has_proot) {
        /* No proot: just exec directly (will likely fail on CPU info) */
        fprintf(stderr, "codebuff: proot not found at %s, "
                "running without CPU fakes\n", PROOT_PATH);
        argv[0] = (char *)CODEBUFF_BINARY;
        execvp(CODEBUFF_BINARY, argv);
        fprintf(stderr, "codebuff: exec failed: %m\n");
        return 1;
    }

    /* Create fake files */
    create_fake_files();

    /* Build proot command with all bind mounts */
    Argv *cmd = argv_new(4 + argc);
    argv_add(cmd, PROOT_PATH);

    /* Bind mount all fake files */
    {
        char bind[512];
        snprintf(bind, sizeof(bind), FAKE_DIR "/stat:/proc/stat");
        argv_add(cmd, "-b"); argv_add(cmd, bind);

        snprintf(bind, sizeof(bind), FAKE_DIR "/cpuinfo:/proc/cpuinfo");
        argv_add(cmd, "-b"); argv_add(cmd, bind);

        snprintf(bind, sizeof(bind), FAKE_DIR "/loadavg:/proc/loadavg");
        argv_add(cmd, "-b"); argv_add(cmd, bind);

        snprintf(bind, sizeof(bind),
                 FAKE_DIR "/cpu-present:/sys/devices/system/cpu/present");
        argv_add(cmd, "-b"); argv_add(cmd, bind);

        snprintf(bind, sizeof(bind),
                 FAKE_DIR "/cpu-online:/sys/devices/system/cpu/online");
        argv_add(cmd, "-b"); argv_add(cmd, bind);
    }

    /* Append codebuff binary + args */
    argv_add(cmd, CODEBUFF_BINARY);
    for (int i = 1; i < argc; i++)
        argv_add(cmd, argv[i]);
    argv_emit(cmd);

    /* Exec under proot */
    execvp(PROOT_PATH, cmd->argv);

    /* Fallback: direct exec */
    fprintf(stderr, "codebuff: proot exec failed (%m), falling back\n");
    argv[0] = (char *)CODEBUFF_BINARY;
    execvp(CODEBUFF_BINARY, argv);
    fprintf(stderr, "codebuff: exec failed: %m\n");
    return 1;
}
