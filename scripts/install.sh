#!/data/data/com.termux/files/usr/bin/bash
#
# install.sh — Codebuff for Termux installer
# Patches the codebuff npm package for Android/Termux compatibility,
# downloads binary, patches glibc compatibility, installs C wrapper.
#
# Usage: bash scripts/install.sh [version]
#   version: codebuff npm version (default: latest)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
WORK_DIR="$TMPDIR/codebuff-termux-build"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log() { echo -e "${GREEN}[✓]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
err()  { echo -e "${RED}[✗]${NC} $1"; exit 1; }

# ---- Config ----
VERSION="${1:-latest}"
BINARY_DIR="${HOME}/.config/manicode"
BINARY_PATH="${BINARY_DIR}/codebuff"
GLIBC_LIB="/data/data/com.termux/files/usr/glibc/lib"
GLIBC_LD="${GLIBC_LIB}/ld-linux-aarch64.so.1"
PROOT="/data/data/com.termux/files/usr/bin/proot"

# ---- Step 1: Determine version ----
if [ "$VERSION" = "latest" ]; then
  log "Fetching latest codebuff version from npm..."
  VERSION=$(npm view codebuff version 2>/dev/null || curl -sL https://registry.npmjs.org/codebuff/latest | node -e "process.stdin.resume(); let d=''; process.stdin.on('data',c=>d+=c); process.stdin.on('end',()=>console.log(JSON.parse(d).version))")
  log "Latest version: $VERSION"
fi

# ---- Step 2: Check dependencies ----
log "Checking dependencies..."
command -v node >/dev/null || err "Node.js is required. Install: pkg install nodejs"
command -v gcc >/dev/null || warn "gcc not found. Will skip C wrapper compilation. Install: pkg install gcc"
command -v patchelf >/dev/null || warn "patchelf not found. Will skip binary patching. Install: pkg install patchelf"

if [ ! -f "$GLIBC_LD" ]; then
  warn "glibc not detected. The codebuff binary requires glibc."
  warn "Install: apt install -y glibc-repo && apt update && apt install -y glibc openssl-glibc"
  warn "Continuing anyway (will fail at binary execution step)..."
fi

if [ ! -x "$PROOT" ]; then
  warn "proot not found. The codebuff CLI may crash with 'Failed to get CPU information'."
  warn "Install: pkg install proot"
fi

# ---- Step 3: Download and patch codebuff npm package ----
rm -rf "$WORK_DIR" && mkdir -p "$WORK_DIR"
log "Downloading codebuff@${VERSION} from npm..."
npm pack "codebuff@${VERSION}" --pack-destination "$WORK_DIR" >/dev/null 2>&1

TGZ=$(ls "$WORK_DIR"/codebuff-*.tgz 2>/dev/null | head -1)
[ -z "$TGZ" ] && err "Failed to download codebuff package"

log "Extracting..."
tar -xzf "$TGZ" -C "$WORK_DIR"

PKG_DIR="$WORK_DIR/package"

# ---- Step 4: Apply patches ----
log "Applying Termux compatibility patches..."

# 4a. Patch package.json: add "android" to supported OS list
node -e "
const pkg = require('$PKG_DIR/package.json');
if (!pkg.os.includes('android')) {
  pkg.os.push('android');
  require('fs').writeFileSync('$PKG_DIR/package.json', JSON.stringify(pkg, null, 2) + '\n');
  console.log('  ✓ Added android to os field in package.json');
}
"

# 4b. Patch index.js: add android-arm64 → linux-arm64 mapping
node -e "
const fs = require('fs');
let js = fs.readFileSync('$PKG_DIR/index.js', 'utf8');
if (!js.includes('android-arm64')) {
  js = js.replace(
    /'win32-x64':/,
    \"'android-arm64': \${packageName}-linux-arm64.tar.gz,  // Termux: same binary as linux-arm64\\n  'win32-x64':\"
  );
  // Fix template literal interpolation
  js = js.replace(/'android-arm64': \$\{packageName\}-linux-arm64\.tar\.gz/, \"'android-arm64': \\\`\\\${packageName}-linux-arm64.tar.gz\\\`\");
  fs.writeFileSync('$PKG_DIR/index.js', js);
  console.log('  ✓ Added android-arm64 platform mapping');
} else {
  console.log('  - android-arm64 mapping already exists');
}
"

# 4c. Fix shebang for Termux
log "Fixing shebang for Termux..."
termux-fix-shebang "$PKG_DIR/index.js" 2>/dev/null || \
  sed -i '1s|^#!/usr/bin/env node|#!/data/data/com.termux/files/usr/bin/env node|' "$PKG_DIR/index.js"

log "Shebang fixed: $(head -1 "$PKG_DIR/index.js")"

# ---- Step 5: Install patched package globally ----
log "Installing patched codebuff@${VERSION} globally..."

cd "$PKG_DIR"
npm pack --pack-destination "$WORK_DIR" >/dev/null 2>&1
PATCHED_TGZ=$(ls "$WORK_DIR"/codebuff-*.tgz 2>/dev/null | head -1)

npm install -g "$PATCHED_TGZ" 2>&1 | tail -3
log "Global install complete"

# ---- Step 6: Fix symlink shebang ----
BIN_PATH="/data/data/com.termux/files/usr/bin/codebuff"
if [ -L "$BIN_PATH" ]; then
  REAL_PATH=$(readlink -f "$BIN_PATH")
  if [ -f "$REAL_PATH" ]; then
    termux-fix-shebang "$REAL_PATH" 2>/dev/null || true
    log "Fixed shebang on bin symlink target"
  fi
fi

# ---- Step 7: Increase download timeout in index.js ----
node -e "
const fs = require('fs');
const installedPath = require('path').join(
  require('child_process').execSync('npm root -g', {encoding:'utf8'}).trim(),
  'codebuff', 'index.js'
);
if (fs.existsSync(installedPath)) {
  let js = fs.readFileSync(installedPath, 'utf8');
  if (js.includes('requestTimeout: 20000') && !js.includes('requestTimeout: 120000')) {
    js = js.replace('requestTimeout: 20000', 'requestTimeout: 120000');
    fs.writeFileSync(installedPath, js);
    console.log('  ✓ Increased download timeout from 20s to 120s');
  }
}
" 2>/dev/null || true

# ---- Step 8: Trigger binary download ----
log "Triggering binary download..."
log "(This fetches ~129MB from GitHub on first run)"
# The JS wrapper will download on first execution
# We use a short timeout and capture any real download progress
timeout 90 codebuff --version 2>/dev/null && log "Binary downloaded successfully" || \
  warn "Binary download may not be complete. Run 'codebuff --version' later to retry."

# ---- Step 9: Patch binary with patchelf ----
if command -v patchelf >/dev/null && [ -f "$BINARY_PATH" ]; then
  log "Patching binary interpreter to glibc..."
  CURRENT_INTERP=$(patchelf --print-interpreter "$BINARY_PATH" 2>/dev/null || echo "")
  if [ "$CURRENT_INTERP" != "$GLIBC_LD" ]; then
    patchelf --set-interpreter "$GLIBC_LD" "$BINARY_PATH"
    log "Interpreter changed to: $GLIBC_LD"
  else
    log "Interpreter already correct: $GLIBC_LD"
  fi
else
  warn "Binary not found at $BINARY_PATH or patchelf missing"
  warn "Run 'codebuff --version' to download, then re-run this script"
fi

# ---- Step 10: Fix glibc linker scripts (.so → symlink) ----
if [ -d "$GLIBC_LIB" ]; then
  log "Fixing glibc linker scripts (libc.so, libm.so, libgcc_s.so)..."
  for f in libc.so libm.so libgcc_s.so; do
    fpath="$GLIBC_LIB/$f"
    if [ -f "$fpath" ] && ! head -c 4 "$fpath" | grep -q $'\x7fELF'; then
      # This is a GNU ld script (text), not an ELF.
      # Find the actual .so.N it points to and replace with symlink
      TARGET=$(grep -oP 'GROUP\s*\(\s*\K[^)]+' "$fpath" 2>/dev/null | tr -d ' ' | tr ',' '\n' | grep -E '\.so\.[0-9]+' | head -1)
      if [ -n "$TARGET" ] && [ -f "$GLIBC_LIB/$TARGET" ]; then
        rm -f "$fpath"
        ln -sf "$TARGET" "$fpath"
        log "  $f → symlink to $TARGET"
      fi
    fi
  done
fi

# ---- Step 11: Compile and install C wrapper ----
if command -v gcc >/dev/null; then
  WRAPPER_SRC="$SCRIPT_DIR/codebuff-wrapper.c"
  WRAPPER_OUT="$SCRIPT_DIR/codebuff-wrapper"
  log "Compiling C wrapper (Bionic ELF)..."
  if gcc -O2 -s -o "$WRAPPER_OUT" "$WRAPPER_SRC" 2>/dev/null; then
    install -m 755 "$WRAPPER_OUT" "$BIN_PATH"
    log "C wrapper installed as $BIN_PATH"
  else
    warn "C wrapper compilation failed. Reverting to JS wrapper as fallback."
  fi
else
  warn "gcc not available. C wrapper not compiled."
  warn "The JS wrapper will be used instead (may have environment issues)."
fi

# ---- Done ----
log ""
log "═══════════════════════════════════════════"
log "  Codebuff for Termux installed!"
log "  Version: $VERSION"
log "  Command: codebuff"
log ""
log "  Binary:   $BINARY_PATH"
log "  Wrapper:  $(file "$BIN_PATH" 2>/dev/null | cut -d: -f2-)"
log ""
log "  First run may download the ~129MB binary."
log "  Run: codebuff"
log "═══════════════════════════════════════════"
