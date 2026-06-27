# Codebuff for Termux

**Codebuff** — AI 编程助手，适配 Android Termux。

## 快速开始

```bash
# 1. 安装依赖
apt install -y glibc-repo && apt update && apt install -y glibc openssl-glibc patchelf
pkg install proot gcc nodejs

# 2. 安装 codebuff（patch npm 包 + binary + C wrapper）
bash scripts/install.sh

# 3. 运行——无需 shell rc 配置
codebuff --version
```

首次运行 `install.sh` 会从 GitHub Releases 自动下载约 129MB 的二进制文件（若网络慢等待即可）。

## 架构概览

```
终端                          codebuff-termux
  │
  ├── /usr/bin/codebuff
  │     └─ C 包装器（Bionic 编译，原生 Termux ELF，~8KB）
  │          ├─ unsetenv(LD_PRELOAD / LD_LIBRARY_PATH / LD_DEBUG)
  │          ├─ 创建假文件（5个，绕过 Android 内核限制）:
  │          │    /proc/stat
  │          │    /proc/cpuinfo
  │          │    /proc/loadavg
  │          │    /sys/devices/system/cpu/present
  │          │    /sys/devices/system/cpu/online
  │          └─ execvp(proot -b f1:/proc/stat -b f2:/proc/cpuinfo ...)
  │                │
  │                ├─ [proot] ptrace 级路径重定向
  │                │    └─ 所有文件读/写操作 → 假文件内容
  │                │
  │                └─ /usr/lib/codebuff/runtime/codebuff
  │                     └─ glibc Bun 运行时（patchelf 改 interpreter）
  │                          └─ glibc ld.so（自动加载 libc.so.6 等）
  │
  └── fallback（无 proot 时）
       └─ 直接 exec(binary)
             └─ os.cpus() 会崩溃（/proc/stat 不可读）
```

**关键设计**：
- **C 包装器**编译为原生 Bionic ELF（不依赖 glibc），清除所有 `LD_*` 环境变量，防止 glibc ld.so 被污染，同时防止 Termux 的 Bionic LD_PRELOAD 库泄露到 glibc 进程
- **proot 方案（vs LD_PRELOAD）**：Bun 的部分系统调用（如 os.cpus() 内部的 openat）不走 libc，而是直接 syscall。LD_PRELOAD 的 hook.so 在 libc 层拦截，被 Bun 绕过。proot 在 ptrace 层拦截，对 Bun 的直接 syscall 同样有效
- **5 个假文件绑定**：不仅仅是 `/proc/stat`，还包括 `/proc/cpuinfo`、`/proc/loadavg`、`/sys/devices/system/cpu/present`、`/sys/devices/system/cpu/online`，覆盖 Bun/Node.js 的 `os.cpus()` 所有读取路径
- **Binary 的 interpreter** 通过 `patchelf` 改为 glibc 的 ld.so
- **glibc 的 `.so` 链接脚本**改为 symlink（`libc.so → libc.so.6` 等），防止 `dlopen("libc.so")` 加载到 ASCII 文本而非 ELF

## 当前状态

| 步骤 | 状态 | 说明 |
|------|------|------|
| npm 安装（patch os 字段） | ✅ **通过** | `npm install -g codebuff` 成功 |
| `android-arm64` 平台映射 | ✅ **通过** | JS 包装器能正确下载 linux-arm64 binary |
| Binary 下载 | ✅ **通过** | GitHub Releases ~124MB |
| glibc 兼容性 | ✅ **通过** | patchelf 改 interpreter → 直接执行 |
| `dlopen` 兼容性 | ✅ **通过** | 修复 `.so` 链接脚本为 symlink |
| 环境变量隔离 | ✅ **通过** | C 包装器清除 `LD_*` |
| `/proc/stat` （os.cpus()） | ✅ **通过** | proot bind mount 5 假文件（stat/cpuinfo/loadavg/present/online） |
| Bun 直接 syscall 绕过 | ✅ **通过** | ptrace 层拦截，Bun 绕不过 |
| 子进程 LD_PRELOAD 污染 | ✅ **通过** | hook.so 的 execve/execvp 自动过滤 + wrapper 清理环境 |
| TUI 文件浏览器 | ✅ **通过** | 目录列表、splash、登录页均正常 |
| 自动更新 | ⚠️ **需手动重装** | C 包装器暂不处理更新逻辑 |
| 非 `/data/` 路径 | ✅ **通过** | `/data/data/` 下也可工作 |

## 解决方案详解

### 问题 1：npm EBADPLATFORM

`package.json` 中 `os: ["darwin", "linux", "win32"]`，Termux 上报 `process.platform = "android"`。

**方案**：添加 `"android"` 到 os 列表（`patches/0001-add-android-os-support.patch`）。

### 问题 2：JS 包装器平台映射缺失

`index.js` 的 `PLATFORM_TARGETS` 没有 `android-arm64`。

**方案**：添加映射 `android-arm64 → codebuff-linux-arm64.tar.gz`（`patches/0002-add-android-platform-mapping.patch`）。

### 问题 3：glibc 兼容性（Binary 运行）

下载的 binary 是 **glibc-linked** (Bun 编译)，Termux 是 Bionic libc。

**方案**：`patchelf --set-interpreter` 将 interpreter 改为 glibc ld.so：
```bash
patchelf --set-interpreter /data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1 \
  ~/.config/manicode/codebuff
```
binary 直接运行时内核自动调用 glibc ld.so，不再需要 `--library-path` 参数。

### 问题 4：dlopen + 链接脚本冲突（"invalid ELF header"）

Termux glibc 包中 `libc.so`、`libm.so` 等是 **GNU ld 脚本（ASCII 文本）** 而非 symlink。Bun binary 内部 `dlopen("libc.so")` 时会加载到非 ELF 文件 → 报 `invalid ELF header`。

**方案**：
1. 将所有 `*.so` 链接脚本改为 symlink → `libc.so.N`（版本化 ELF）
2. C 包装器清除 `LD_LIBRARY_PATH` 防止意外泄露

### 问题 5：环境变量污染

父 shell 如果设了 `LD_LIBRARY_PATH`，会泄露给 glibc ld.so，导致错误加载。

**方案**：C 包装器内 `unsetenv("LD_LIBRARY_PATH")`，不依赖任何 shell rc 文件。

### 问题 6：Android 内核限制 /proc/stat 不可读 + Bun 直接 syscall 绕过 libc

Android 11+ 内核禁止非特权进程读取 `/proc/stat`、`/proc/cpuinfo`、`/proc/loadavg`、`/sys/devices/system/cpu/*` 等。Bun 的 libuv 调用 `uv_cpu_info()` → `os.cpus()` → openat 直接 syscall → EACCES → `Failed to get CPU information`（ERR_SYSTEM_ERROR）。

**难点**：Bun 的 `os.cpus()` 不走 libc 的 `open()`/`fopen()`，而是直接 `syscall(SYS_openat, ...)`。
这意味着 LD_PRELOAD hook.so 在 libc 层拦截无效 — Bun 直接绕过了。

**方案**：使用 proot 在 ptrace 层拦截所有 syscall，绑定 5 个假文件：
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

C 包装器自动检测 proot 可用性并执行上述操作。无 proot 时 fallback 到直接执行（os.cpus() 会崩溃）。

**关于 hook.so**：`tools/hook.c` 原用于 LD_PRELOAD 方案，已在 `execve()` 和 `execvp()` 中添加 LD_PRELOAD 过滤逻辑，防止子进程（Bionic shell）继承 glibc hook 导致崩溃。当前 proot 方案中 hook.so 不再通过 LD_PRELOAD 加载，但保留为安装包组件，供未来备选或 freebuff 同类方案使用。

### 问题 7：/data/ 目录限制（SELinux）

Android 的 SELinux 策略限制非特权进程读取 `/data/` 目录内容。某些 Bun 版本在启动时扫描父目录时可能触发 `CouldntReadCurrentDirectory`。

**方案**：此问题在 codebuff 特定版本中影响有限。C 包装器通过 proot 可能缓解该问题。

## 项目结构

```
codebuff-termux/
├── scripts/
│   ├── install.sh                # 全自动安装脚本
│   ├── codebuff-wrapper.c        # C 包装器源码（proot 绑定 5 假文件）
│   └── codebuff-wrapper-nopreload.c  # 旧版 (备用, LD_PRELOAD 方案)
├── patches/
│   ├── 0001-add-android-os-support.patch
│   └── 0002-add-android-platform-mapping.patch
├── Makefile
└── README.md
```

### `scripts/install.sh`
全自动完成：
1. 查询 npm registry 获取最新版本
2. `npm pack` 下载 codebuff 包
3. 修改 `package.json` → 添加 `android` 到 os
4. 修改 `index.js` → 添加 `android-arm64` 映射
5. `termux-fix-shebang` → shebang 修复
6. 增加下载超时（20s → 120s）
7. 重新打包，全局安装
8. 触发 binary 下载（GitHub Releases ~129MB）
9. `patchelf` → 修改 interpreter 为 glibc ld.so
10. 修复 glibc lib 链接脚本（`.so` → symlink）
11. 编译 C 包装器，安装为 `/usr/bin/codebuff`

### `scripts/codebuff-wrapper.c`
Bionic 编译的 C 包装器 (~8KB)，逻辑：
- `unsetenv()` 清除 `LD_PRELOAD`、`LD_LIBRARY_PATH`、`LD_DEBUG` — 防止环境变量污染 glibc 加载
- 检测 proot 可用性，创建 5 个假文件（stat/cpuinfo/loadavg/present/online）
- 构建 proot argv：`proot -b <5 个 bind mount>` → `codebuff binary` → `原始参数`
- `execvp()` 运行 proot
- 不依赖 bash、zsh、node 或任何 rc 文件
- 无 proot 时 fallback 到直接 exec（os.cpus() 会崩溃）

**为什么不用 LD_PRELOAD？** Bun 的 `os.cpus()` 走直接 syscall 绕过 libc，LD_PRELOAD hook 无效。
proot 在 ptrace 层拦截，对所有 syscall 方式一视同仁。作为代价，IO 密集型任务有 ~5-15% 性能损失，
TUI 交互场景无感。

## 依赖

| 包 | 必要性 | 安装 |
|----|--------|------|
| `nodejs` | ✅ 必需 | `pkg install nodejs` |
| `glibc` | ✅ 必需 | `apt install -y glibc-repo && apt update && apt install -y glibc` |
| `openssl-glibc` | ✅ 必需 | `apt install -y openssl-glibc` |
| `patchelf` | ✅ 必需 | `pkg install patchelf` |
| `gcc` | ✅ 必需（编译 C wrapper） | `pkg install gcc` |
| `proot` | ✅ 强烈推荐 | `pkg install proot`（无 proot 时 os.cpus() 崩溃） |

## 已知限制

1. **自动更新**：codebuff 自动更新需 JS 包装器的 spawn 路径（C 包装器暂不处理更新，需手动重装）
2. **proot 依赖**：无 proot 时 CLI 会因 `os.cpus()` 崩溃（`--version` 不受影响）
3. **glibc 依赖**：需要额外安装 glibc + openssl-glibc
4. **网络要求**：首次下载 ~129MB
5. **proot 性能开销**：ptrace 模式对 IO 密集型任务引入 ~5-15% 性能损失（交互式 TUI 场景无感）
6. **hook.so 未使用**：`tools/hook.c` 的 LD_PRELOAD 拦截方案因 Bun 直接 syscall 无效，当前方案用 proot 替代。hook.so 保留为安装包组件供 freebuff 等同族项目使用

## 链接

- [Codebuff on npm](https://www.npmjs.com/package/codebuff)
- [Codebuff GitHub](https://github.com/CodebuffAI/codebuff)
- [Codebuff Community Releases](https://github.com/CodebuffAI/codebuff-community/releases)
- [opencode-termux](https://github.com/Hope2333/opencode-termux)
