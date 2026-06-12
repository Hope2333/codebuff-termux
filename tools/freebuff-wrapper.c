/**
 * freebuff-wrapper.c — Native Termux (Bionic) ELF wrapper for freebuff
 *
 * Compiles against Bionic. Cleans environment, sets LD_PRELOAD to a
 * glibc-compatible hook.so that redirects /proc/stat reads to a fake
 * file (bypassing Android 11+ kernel restriction on /proc/stat).
 *
 * Compile:
 *   gcc -O2 -s -o freebuff-wrapper freebuff-wrapper.c
 *   install -m 755 freebuff-wrapper /data/data/com.termux/files/usr/bin/freebuff
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *binary =
    "/data/data/com.termux/files/home/.config/manicode/freebuff";
static const char *hook_so =
    "/data/data/com.termux/files/usr/lib/libfreebuff-hook.so";
static const char *fake_stat_path =
    "/data/data/com.termux/files/usr/tmp/.freebuff-proc-stat";

static void ensure_fake_stat(void) {
    if (access(fake_stat_path, R_OK) == 0)
        return;  /* already exists */
    const char *content =
        "cpu  0 0 0 0 0 0 0 0 0 0\n"
        "cpu0 0 0 0 0 0 0 0 0 0 0\n"
        "cpu1 0 0 0 0 0 0 0 0 0 0\n"
        "cpu2 0 0 0 0 0 0 0 0 0 0\n"
        "cpu3 0 0 0 0 0 0 0 0 0 0\n"
        "cpu4 0 0 0 0 0 0 0 0 0 0\n"
        "cpu5 0 0 0 0 0 0 0 0 0 0\n"
        "cpu6 0 0 0 0 0 0 0 0 0 0\n"
        "cpu7 0 0 0 0 0 0 0 0 0 0\n"
        "intr 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
        "ctxt 0\n"
        "btime 0\n"
        "processes 0\n"
        "procs_running 1\n"
        "procs_blocked 0\n"
        "softirq 0 0 0 0 0 0 0 0 0 0\n";
    FILE *f = fopen(fake_stat_path, "w");
    if (f) {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
        chmod(fake_stat_path, 0644);
    }
}

int main(int argc, char *argv[], char *envp[]) {
    /* Strip environment variables that poison glibc ld.so */
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("LD_PRELOAD");
    unsetenv("LD_DEBUG");
    unsetenv("LD_BIND_NOW");
    unsetenv("LD_ORIGIN_PATH");
    unsetenv("LD_RUN_PATH");
    unsetenv("GLIBC_LD_LIBRARY_PATH");

    /* Ensure fake /proc/stat exists */
    ensure_fake_stat();

    /* Set LD_PRELOAD to our redirect hook (if available) */
    if (access(hook_so, R_OK) == 0)
        setenv("LD_PRELOAD", hook_so, 1);

    /* Build argv: binary + user args */
    char **new_argv = malloc((argc + 1) * sizeof(char *));
    if (!new_argv) return 1;
    new_argv[0] = (char *)binary;
    for (int i = 1; i < argc; i++) new_argv[i] = argv[i];
    new_argv[argc] = NULL;

    execve(binary, new_argv, environ);
    fprintf(stderr, "freebuff-wrapper: execve %s failed: %m\n", binary);
    return 1;
}
