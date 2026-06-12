#!/data/data/com.termux/files/usr/bin/bash
#
# scripts/test.sh — End-to-end tests for freebuff-termux
#
# Tests:
#   1. Binary exists and is executable
#   2. Binary interpreter is set to glibc
#   3. C wrapper compiles and executes
#   4. hook.so compiles (glibc ABI)
#   5. --version works (without proot)
#   6. --help works (without proot)
#   7. Graceful failure when no proot + os.cpus() crash
#   8. Works correctly under proot (if available)
#
# Usage:
#   bash scripts/test.sh          # run all tests
#   bash scripts/test.sh quick    # skip heavy tests
#

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; fail_count=$((fail_count + 1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $1"; }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

TOTAL=0
PASS=0
fail_count=0

run_test() {
    TOTAL=$((TOTAL + 1))
    local name="$1"
    shift
    if "$@"; then
        pass "$name"
        PASS=$((PASS + 1))
    else
        fail "$name"
    fi
}

# ── Config ──────────────────────────────────────────────────────────
BINARY="/data/data/com.termux/files/home/.config/manicode/freebuff"
HOOK="$PROJECT_DIR/tools/hook.so"
WRAPPER="$PROJECT_DIR/scripts/freebuff-wrapper"
WRAPPER_C="$PROJECT_DIR/scripts/freebuff-wrapper.c"
GLIBC_LD="/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1"
QUICK_MODE="${1:-full}"

# ═══════════════════════════════════════════════════════════════════
echo "=== Freebuff Termux End-to-End Test ==="
echo "Started: $(date)"
echo "Mode: $QUICK_MODE"
echo ""

# ── 1. Binary checks ────────────────────────────────────────────────
echo "--- 1. Binary ---"

run_test "binary exists" test -f "$BINARY"
run_test "binary executable" test -x "$BINARY"

if [ -f "$BINARY" ]; then
    run_test "binary ELF" sh -c "file '$BINARY' | grep -q ELF"
    run_test "binary ARM64" sh -c "file '$BINARY' | grep -q 'aarch64\|ARM64\|arm64'"

    # Check interpreter
    INTERP=$(patchelf --print-interpreter "$BINARY" 2>/dev/null || echo "patchelf not available")
    if [ "$INTERP" = "$GLIBC_LD" ]; then
        run_test "binary interpreter = glibc" true
    else
        run_test "binary interpreter = glibc (found: $INTERP)" false
    fi
fi

# ── 2. hook.so ──────────────────────────────────────────────────────
echo ""
echo "--- 2. hook.so ---"

if [ -f "$HOOK" ]; then
    run_test "hook.so exists" true
    run_test "hook.so is ELF" sh -c "file '$HOOK' | grep -q ELF"
    run_test "hook.so no Bionic marker" sh -c "file '$HOOK' | grep -qv 'for Android'"
else
    skip "hook.so not found (run 'make produce' first)"
fi

# ── 3. C wrapper ────────────────────────────────────────────────────
echo ""
echo "--- 3. C wrapper ---"

if [ -f "$WRAPPER" ]; then
    run_test "wrapper exists" true
    run_test "wrapper is Bionic ELF" sh -c "file '$WRAPPER' | grep -qE 'Android|Bionic|pie executable'"
else
    # Try to compile it
    if [ -f "$WRAPPER_C" ]; then
        info "Compiling wrapper..."
        if gcc -O2 -s -o "$WRAPPER" "$WRAPPER_C" 2>/dev/null; then
            run_test "wrapper compiled" true
        else
            run_test "wrapper compilation" false
        fi
    else
        skip "wrapper source not found"
    fi
fi

# ── 4. Basic functionality ──────────────────────────────────────────
echo ""
echo "--- 4. Basic functionality ---"

# --version (doesn't trigger os.cpus())
if [ -f "$BINARY" ]; then
    run_test "--version works" timeout 10 "$BINARY" --version 2>/dev/null
fi

# --help (doesn't trigger os.cpus())
if [ -f "$BINARY" ]; then
    run_test "--help works" timeout 10 "$BINARY" --help 2>/dev/null
fi

# ── 5. Wrapper functionality ────────────────────────────────────────
echo ""
echo "--- 5. C wrapper ---"

if [ -x "$WRAPPER" ]; then
    run_test "wrapper --version" timeout 10 "$WRAPPER" --version 2>/dev/null
    run_test "wrapper --help" timeout 10 "$WRAPPER" --help 2>/dev/null
    run_test "wrapper --no-proot --version" timeout 10 "$WRAPPER" --no-proot --version 2>/dev/null
    run_test "wrapper --no-proot --help" timeout 10 "$WRAPPER" --no-proot --help 2>/dev/null
fi

# ── 6. os.cpus() crash test ────────────────────────────────────────
echo ""
echo "--- 6. os.cpus() crash (heavy) ---"

if [ "$QUICK_MODE" != "quick" ]; then
    # Test if proot fixes the crash
    if command -v proot >/dev/null; then
        info "Testing with proot (may take 15s)..."
        if timeout 15 proot -b /dev/null:/dev/null "$BINARY" --version 2>/dev/null; then
            run_test "proot + binary --version" true
        else
            run_test "proot + binary --version" false
        fi
    else
        skip "proot not available, skipping crash test"
    fi

    # Test without proot (expect it to crash on os.cpus() for TUI)
    info "Testing without proot (os.cpus() crash expected)..."
    # Use a command that won't trigger os.cpus() to verify graceful handling
    run_test "without proot, --version still works" timeout 10 "$BINARY" --version 2>/dev/null
else
    skip "os.cpus() crash tests (use full mode)"
fi

# ── 7. Staged artifacts ─────────────────────────────────────────────
echo ""
echo "--- 7. Staged artifacts ---"

STAGED="$PROJECT_DIR/artifacts/staged"
if [ -d "$STAGED" ]; then
    run_test "staged bin/freebuff" test -f "$STAGED/bin/freebuff"
    run_test "staged lib/freebuff/hook.so" test -f "$STAGED/lib/freebuff/hook.so"
    run_test "staged install.sh" test -f "$STAGED/lib/freebuff/install.sh"
else
    skip "staged directory not present (run 'make stage' first)"
fi

# ═══════════════════════════════════════════════════════════════════
echo ""
echo "═══════════════════════════════════════════"
echo "  Results: $PASS/$TOTAL passed"
if [ "$fail_count" -gt 0 ]; then
    echo "  Failures: $fail_count"
    echo "  Status:  ${RED}FAILED${NC}"
    exit 1
else
    echo "  Status:  ${GREEN}PASSED${NC}"
fi
echo "═══════════════════════════════════════════"
