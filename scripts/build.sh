#!/data/data/com.termux/files/usr/bin/bash
#
# scripts/build.sh — Stage artifacts for distribution
#
# Collects all build outputs into a clean artifacts/staged/ directory.
# This is the "stage" step in the produce → stage → install pipeline.
#
# Usage:
#   bash scripts/build.sh     # stage artifacts from tools/ + scripts/
#
# Output: artifacts/staged/
#   bin/codebuff              ← C wrapper (installed to /usr/bin)
#   lib/codebuff/hook.so      ← LD_PRELOAD hook
#   lib/codebuff/install.sh   ← full installer
#   lib/codebuff/patches/     ← npm patches
#   lib/codebuff/scripts/     ← helper scripts
#   lib/codebuff/Makefile     ← build system fragment
#

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
STAGED="$PROJECT_DIR/artifacts/staged"
BUILD_LOG="$PROJECT_DIR/artifacts/build.log"
mkdir -p "$PROJECT_DIR/artifacts"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[✓]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
err()  { echo -e "${RED}[✗]${NC} $1"; exit 1; }

log "=== Codebuff Termux — Stage ==="
echo "Started: $(date)" > "$BUILD_LOG"

# Clean
rm -rf "$STAGED"
mkdir -p "$STAGED/bin"
mkdir -p "$STAGED/lib/codebuff/runtime"
mkdir -p "$STAGED/lib/codebuff/patches"
mkdir -p "$STAGED/lib/codebuff/scripts"

# ── 1. C wrapper ────────────────────────────────────────────────────
WRAPPER_SRC="$PROJECT_DIR/scripts/codebuff-wrapper.c"
WRAPPER_OUT="$STAGED/bin/codebuff"
if [ -f "$WRAPPER_SRC" ]; then
    log "Building C wrapper..."
    if gcc -O2 -s -o "$WRAPPER_OUT" "$WRAPPER_SRC" 2>>"$BUILD_LOG"; then
        log "  → bin/codebuff ($(du -h "$WRAPPER_OUT" | cut -f1))"
    else
        err "C wrapper build failed. See $BUILD_LOG"
    fi
else
    err "codebuff-wrapper.c not found"
fi

# ── 2. Runtime binary ───────────────────────────────────────────────
RUNTIME_SRC="${HOME}/.config/manicode/codebuff"
RUNTIME_OUT="$STAGED/lib/codebuff/runtime/codebuff"
if [ -f "$RUNTIME_SRC" ]; then
	cp -a "$RUNTIME_SRC" "$RUNTIME_OUT"
	chmod 755 "$RUNTIME_OUT"
	log "  → lib/codebuff/runtime/codebuff ($(du -h "$RUNTIME_OUT" | cut -f1))"
else
	err "runtime binary not found at $RUNTIME_SRC (run 'make produce' first)"
fi

# ── 3. hook.so ──────────────────────────────────────────────────────
HOOK_SRC="$PROJECT_DIR/tools/hook.c"
HOOK_OUT="$STAGED/lib/codebuff/hook.so"
GLIBC_INC="/data/data/com.termux/files/usr/glibc/include"
GLIBC_LIB="/data/data/com.termux/files/usr/glibc/lib"

if [ -f "$HOOK_SRC" ]; then
    log "Building hook.so (glibc)..."
    if gcc -fPIC -shared -o "$HOOK_OUT" "$HOOK_SRC" \
         -I"$GLIBC_INC" -L"$GLIBC_LIB" \
         -nostdlib -lc -ldl \
         -Wl,-rpath,"$GLIBC_LIB" \
         2>>"$BUILD_LOG"; then
        # Strip to reduce size
        strip "$HOOK_OUT" 2>/dev/null || true
        log "  → lib/codebuff/hook.so ($(du -h "$HOOK_OUT" | cut -f1))"
    else
        warn "hook.so build failed — will use fallback"
    fi
else
    warn "hook.c not found at $HOOK_SRC"
fi

# ── 3. Install script ───────────────────────────────────────────────
cp "$PROJECT_DIR/scripts/install.sh" "$STAGED/lib/codebuff/install.sh"
chmod +x "$STAGED/lib/codebuff/install.sh"
log "  → lib/codebuff/install.sh"

# ── 4. Patches ──────────────────────────────────────────────────────
if [ -d "$PROJECT_DIR/patches" ]; then
    cp "$PROJECT_DIR/patches"/*.patch "$STAGED/lib/codebuff/patches/" 2>/dev/null || true
    log "  → lib/codebuff/patches/ ($(ls "$STAGED/lib/codebuff/patches/" 2>/dev/null | wc -l) files)"
fi

# ── 5. Helper scripts ───────────────────────────────────────────────
cp "$PROJECT_DIR/scripts/codebuff-wrapper.c" "$STAGED/lib/codebuff/scripts/"
log "  → lib/codebuff/scripts/codebuff-wrapper.c"

# Copy produce-local.sh for development
cp "$PROJECT_DIR/tools/produce-local.sh" "$STAGED/lib/codebuff/scripts/produce-local.sh" 2>/dev/null || true
log "  → lib/codebuff/scripts/produce-local.sh"

# ── 6. Makefile fragment ────────────────────────────────────────────
cp "$PROJECT_DIR/Makefile" "$STAGED/lib/codebuff/Makefile" 2>/dev/null || true
log "  → lib/codebuff/Makefile"

# ── Summary ─────────────────────────────────────────────────────────
log ""
log "═══════════════════════════════════════════"
log "  Staged to: $STAGED"
log ""
log "  $(du -sh "$STAGED" | cut -f1) total"
log "  $(find "$STAGED" -type f | wc -l) files"
log ""
log "  Tree:"
find "$STAGED" -type f | sed 's|^.*/staged/|  |' | sort
log ""
log "  Install:  cp -r artifacts/staged/bin/* /usr/bin/"
log "            cp -r artifacts/staged/lib/*  /usr/lib/"
log "═══════════════════════════════════════════"
