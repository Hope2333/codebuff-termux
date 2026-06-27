# Codebuff for Termux

**Codebuff** — An AI coding assistant, adapted for Android Termux.

## Quick Start

```bash
# 1. Install dependencies
apt install -y glibc-repo && apt update && apt install -y glibc openssl-glibc patchelf
pkg install proot gcc nodejs

# 2. Install codebuff (patches npm package + binary + C wrapper)
bash scripts/install.sh

# 3. Run — no shell rc configuration needed
codebuff --version
```

The first run of `install.sh` automatically downloads about 129MB of binary from GitHub Releases. If your connection is slow, just wait.

## Architecture Overview

```
Terminal                         codebuff-termux
  │
  ├── /usr/bin/codebuff
  │     └─ C wrapper (Bionic compiled, native Termux ELF, ~8KB)
  │          ├─ unsetenv(LD_PRELOAD / LD_LIBRARY_PATH / LD_DEBUG)
  │          ├─ Create fake files (5, to bypass Android kernel restrictions):
  │          │    /proc/stat
  │          │    /proc/cpuinfo
  │          │    /proc/loadavg
  │          │    /sys/devices/system/cpu/present
  │          │    /sys/devices/system/cpu/online
  │          └─ execvp(proot -b f1:/proc/stat -b f2:/proc/cpuinfo ...)
  │                │
  │                ├─ [proot] ptrace-level path redirection
  │                │    └─ All file read/write operations → fake file content
  │                │
  │                └─ /usr/lib/codebuff/runtime/codebuff
  │                     └─ glibc Bun runtime (patchelf modified interpreter)
  │                          └─ glibc ld.so (automatically loads libc.so.6 etc.)
  │
  └── Fallback (when proot is unavailable)
       └─ Direct exec(binary)
             └─ os.cpus() crashes (/proc/stat unreadable)
```

**Key design points**:
- **C wrapper** is compiled as a native Bionic ELF (no glibc dependency). It clears all `LD_*` environment variables to prevent contamination of glibc ld.so, and also stops Termux's Bionic LD_PRELOAD libraries from leaking into the glibc process.
- **proot approach (vs LD_PRELOAD)**: Bun's system calls (like openat inside os.cpus()) don't go through libc — they use direct syscalls. An LD_PRELOAD hook.so intercepts at the libc layer, which Bun bypasses. proot intercepts at the ptrace layer, so it works on Bun's direct syscalls too.
- **5 fake file bindings**: Not just `/proc/stat`, but also `/proc/cpuinfo`, `/proc/loadavg`, `/sys/devices/system/cpu/present`, `/sys/devices/system/cpu/online`. Covers all the paths Bun/Node.js reads during `os.cpus()`.
- **Binary interpreter** changed to glibc's ld.so via `patchelf`.
- **glibc `.so` linker scripts** converted to symlinks (`libc.so → libc.so.6` etc.) so `dlopen("libc.so")` loads an ELF instead of ASCII text.

## Current Status

| Step | Status | Notes |
|------|--------|-------|
| npm install (patched os field) | ✅ **Pass** | `npm install -g codebuff` succeeds |
| `android-arm64` platform mapping | ✅ **Pass** | JS wrapper downloads the correct linux-arm64 binary |
| Binary download | ✅ **Pass** | GitHub Releases ~124MB |
| glibc compatibility | ✅ **Pass** | patchelf changes interpreter, binary runs directly |
| `dlopen` compatibility | ✅ **Pass** | Fixed `.so` linker scripts to symlinks |
| Environment variable isolation | ✅ **Pass** | C wrapper clears `LD_*` |
| `/proc/stat` (os.cpus()) | ✅ **Pass** | proot bind mounts 5 fake files (stat/cpuinfo/loadavg/present/online) |
| Bun direct syscall bypass | ✅ **Pass** | ptrace-level interception, Bun can't bypass |
| Child process LD_PRELOAD pollution | ✅ **Pass** | hook.so execve/execvp auto-filter + wrapper cleans environment |
| TUI file browser | ✅ **Pass** | Directory listing, splash, login page all work |
| Auto-update | ⚠️ **Manual reinstall needed** | C wrapper doesn't handle update logic yet |
| Non-`/data/` path | ✅ **Pass** | Works under `/data/data/` too |

## Solution Details

### Problem 1: npm EBADPLATFORM

The `package.json` lists `os: ["darwin", "linux", "win32"]`, but Termux reports `process.platform = "android"`.

**Solution**: Add `"android"` to the os list (`patches/0001-add-android-os-support.patch`).

### Problem 2: JS wrapper platform mapping missing

The `PLATFORM_TARGETS` map in `index.js` doesn't include `android-arm64`.

**Solution**: Add the mapping `android-arm64 → codebuff-linux-arm64.tar.gz` (`patches/0002-add-android-platform-mapping.patch`).

### Problem 3: glibc compatibility (binary execution)

The downloaded binary is **glibc-linked** (compiled by Bun), but Termux uses Bionic libc.

**Solution**: Use `patchelf --set-interpreter` to change the interpreter to glibc's ld.so:
```bash
patchelf --set-interpreter /data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1 \
  ~/.config/manicode/codebuff
```
When the binary runs directly, the kernel automatically invokes glibc ld.so. No need for `--library-path`.

### Problem 4: dlopen + linker script conflict ("invalid ELF header")

In Termux's glibc package, `libc.so`, `libm.so`, etc. are **GNU ld scripts (ASCII text)** instead of symlinks. When Bun's binary calls `dlopen("libc.so")`, it loads a non-ELF file and gets `invalid ELF header`.

**Solution**:
1. Convert all `*.so` linker scripts to symlinks → `libc.so.N` (versioned ELF)
2. The C wrapper clears `LD_LIBRARY_PATH` to prevent accidental leaks

### Problem 5: Environment variable pollution

If the parent shell has `LD_LIBRARY_PATH` set, it leaks to glibc ld.so and causes incorrect loading.

**Solution**: Call `unsetenv("LD_LIBRARY_PATH")` inside the C wrapper. No shell rc file dependency.

### Problem 6: Android kernel restriction (/proc/stat unreadable) + Bun direct syscall bypassing libc

Android 11+ kernels prevent unprivileged processes from reading `/proc/stat`, `/proc/cpuinfo`, `/proc/loadavg`, `/sys/devices/system/cpu/*`, etc. Bun's libuv calls `uv_cpu_info()` → `os.cpus()` → openat direct syscall → EACCES → `Failed to get CPU information` (ERR_SYSTEM_ERROR).

**The challenge**: Bun's `os.cpus()` doesn't go through libc's `open()` or `fopen()`. It calls `syscall(SYS_openat, ...)` directly. An LD_PRELOAD hook.so at the libc layer is ineffective — Bun bypasses it entirely.

**Solution**: Use proot to intercept at the ptrace level. Bind 5 fake files:
```bash
FAKE_DIR=/data/data/com.termux/files/usr/tmp/.codebuff-fake
proot \
  -b $FAKE_DIR/stat:/proc/stat \
  -b $FAKE_DIR/cpuinfo:/proc/cpuinfo \
  -b $FAKE_DIR/loadavg:/proc/loadavg \
  -b $FAKE_DIR/cpu-present:/sys/devices/system/cpu/present \
  -b $FAKE_DIR/cpu-online:/sys/devices/system/cpu/online \
  codebuff
```

The C wrapper automatically checks for proot availability and runs the above. Without proot, it falls back to direct execution (os.cpus() will crash).

**About hook.so**: The `tools/hook.c` file was originally for the LD_PRELOAD approach. It already has LD_PRELOAD filtering logic in `execve()` and `execvp()` to prevent child processes (Bionic shell) from inheriting glibc hooks and crashing. In the current proot approach, hook.so is no longer loaded via LD_PRELOAD, but it's kept as a package component for future alternatives or similar projects like freebuff.

### Problem 7: /data/ directory restriction (SELinux)

Android's SELinux policy restricts unprivileged processes from reading `/data/` directory contents. Some Bun versions may trigger `CouldntReadCurrentDirectory` when scanning parent directories at startup.

**Solution**: This issue has limited impact on specific codebuff versions. The C wrapper may mitigate it through proot.

## Project Structure

```
codebuff-termux/
├── scripts/
│   ├── install.sh                # Fully automated install script
│   ├── codebuff-wrapper.c        # C wrapper source (proot bind 5 fake files)
│   └── codebuff-wrapper-nopreload.c  # Legacy version (backup, LD_PRELOAD approach)
├── patches/
│   ├── 0001-add-android-os-support.patch
│   └── 0002-add-android-platform-mapping.patch
├── Makefile
└── README.md
```

### `scripts/install.sh`

Fully automated 11-step process:
1. Query npm registry for the latest version
2. `npm pack` downloads the codebuff package
3. Modify `package.json` — add `android` to os
4. Modify `index.js` — add `android-arm64` mapping
5. `termux-fix-shebang` — fix shebang
6. Increase download timeout (20s → 120s)
7. Repackage and install globally
8. Trigger binary download (GitHub Releases ~129MB)
9. `patchelf` — change interpreter to glibc ld.so
10. Fix glibc lib linker scripts (`.so` → symlink)
11. Compile C wrapper, install as `/usr/bin/codebuff`

### `scripts/codebuff-wrapper.c`

A C wrapper compiled with Bionic (~8KB). Its logic:
- `unsetenv()` clears `LD_PRELOAD`, `LD_LIBRARY_PATH`, `LD_DEBUG` — prevents environment variable pollution of glibc loading
- Checks proot availability, creates 5 fake files (stat/cpuinfo/loadavg/present/online)
- Builds proot argv: `proot -b <5 bind mounts>` → `codebuff binary` → `original arguments`
- `execvp()` runs proot
- No dependency on bash, zsh, node, or any rc file
- Falls back to direct exec when proot is unavailable (os.cpus() will crash)

**Why not LD_PRELOAD?** Bun's `os.cpus()` uses direct syscalls that bypass libc, so LD_PRELOAD hooks are ineffective. proot intercepts at the ptrace level and treats all syscall paths equally. The tradeoff is roughly 5-15% performance overhead for IO-intensive tasks, which is imperceptible during TUI interactions.

## Dependencies

| Package | Required | Install |
|---------|----------|---------|
| `nodejs` | ✅ Required | `pkg install nodejs` |
| `glibc` | ✅ Required | `apt install -y glibc-repo && apt update && apt install -y glibc` |
| `openssl-glibc` | ✅ Required | `apt install -y openssl-glibc` |
| `patchelf` | ✅ Required | `pkg install patchelf` |
| `gcc` | ✅ Required (compile C wrapper) | `pkg install gcc` |
| `proot` | ✅ Strongly recommended | `pkg install proot` (without proot, os.cpus() crashes) |

## Known Limitations

1. **Auto-update**: Codebuff's auto-update needs the JS wrapper's spawn path. The C wrapper doesn't handle updates yet, so manual reinstall is required.
2. **proot dependency**: Without proot, the CLI crashes from `os.cpus()` (though `--version` works fine).
3. **glibc dependency**: Requires installing glibc + openssl-glibc separately.
4. **Network requirement**: First download is about 129MB.
5. **proot performance overhead**: ptrace mode introduces roughly 5-15% performance loss on IO-intensive tasks (imperceptible for interactive TUI use).
6. **hook.so unused**: The `tools/hook.c` LD_PRELOAD interception approach is ineffective because of Bun's direct syscalls. The current approach uses proot instead. hook.so is kept as a package component for sibling projects like freebuff.

## Links

- [Codebuff on npm](https://www.npmjs.com/package/codebuff)
- [Codebuff GitHub](https://github.com/CodebuffAI/codebuff)
- [Codebuff Community Releases](https://github.com/CodebuffAI/codebuff-community/releases)
- [opencode-termux](https://github.com/Hope2333/opencode-termux)
