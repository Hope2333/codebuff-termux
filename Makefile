#!/data/data/com.termux/files/usr/bin/make
# Freebuff for Termux — build system
SHELL := /data/data/com.termux/files/usr/bin/bash
.DEFAULT_GOAL := help

PROJECT_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
TOOLS_DIR   := $(PROJECT_DIR)/tools
SCRIPTS_DIR := $(PROJECT_DIR)/scripts
ARTIFACTS   := $(PROJECT_DIR)/artifacts
STAGED      := $(ARTIFACTS)/staged

HOOK_SRC    := $(TOOLS_DIR)/hook.c
HOOK_OUT    := $(TOOLS_DIR)/hook.so
WRAPPER_SRC := $(SCRIPTS_DIR)/freebuff-wrapper.c
WRAPPER_OUT := $(SCRIPTS_DIR)/freebuff-wrapper

BINARY_DIR  := $(HOME)/.config/manicode
BINARY_PATH := $(BINARY_DIR)/freebuff

GLIBC_INC   := /data/data/com.termux/files/usr/glibc/include
GLIBC_LIB   := /data/data/com.termux/files/usr/glibc/lib

# ═══════════════════════════════════════════════════════════════════
.PHONY: help version deps produce stage install test clean dev

help:
	@echo "Freebuff for Termux — build system"
	@echo ""
	@echo "Targets:"
	@echo "  make help         Show this help"
	@echo "  make version      Show version info"
	@echo "  make deps         Check required dependencies"
	@echo "  make produce      Download binary + compile hook + wrapper"
	@echo "  make stage        Stage artifacts to $(STAGED)"
	@echo "  make install      Full install (produce + stage + install to system)"
	@echo "  make test         End-to-end validation"
	@echo "  make clean        Remove build artifacts"
	@echo "  make dev          produce + stage + test (fast iteration)"
	@echo ""
	@echo "Quick start:"
	@echo "  make dev     # build and test"
	@echo "  make install # install to system"
	@echo ""

# ═══════════════════════════════════════════════════════════════════
version:
	@echo "Freebuff Termux"
	@echo "  Project: $(PROJECT_DIR)"
	@echo "  Binary:  $(BINARY_PATH)"
	@echo "  hook.so: $(HOOK_OUT)"
	@echo "  Wrapper: $(WRAPPER_OUT)"
	@-test -f "$(BINARY_PATH)" && echo "  Version: $$(timeout 5 $(BINARY_PATH) --version 2>/dev/null || echo '(unknown)')" || true
	@-test -f "$(BINARY_PATH)" && echo "  Size:    $$(du -h "$(BINARY_PATH)" | cut -f1)" || true
	@echo "  glibc:   $(GLIBC_LIB)"

# ═══════════════════════════════════════════════════════════════════
deps:
	@echo "Checking dependencies..."
	@for cmd in gcc npm patchelf; do \
		if command -v $$cmd >/dev/null 2>&1; then \
			echo "  [✓] $$cmd"; \
		else \
			echo "  [✗] $$cmd — install: pkg install $$cmd"; \
		fi; \
	done
	@if [ -d "$(GLIBC_LIB)" ]; then \
		echo "  [✓] glibc ($(GLIBC_LIB))"; \
	else \
		echo "  [✗] glibc — install: apt install -y glibc-repo && apt update && apt install -y glibc"; \
	fi
	@if [ -f "$(BINARY_PATH)" ]; then \
		echo "  [✓] binary ($(BINARY_PATH) — $$(du -h "$(BINARY_PATH)" | cut -f1))"; \
	else \
		echo "  [ ] binary (run 'make produce' to download)"; \
	fi

# ═══════════════════════════════════════════════════════════════════
produce:
	@echo "=== Produce ==="
	@bash "$(TOOLS_DIR)/produce-local.sh"

stage: produce
	@echo "=== Stage ==="
	@bash "$(SCRIPTS_DIR)/build.sh"

# ═══════════════════════════════════════════════════════════════════
install: stage
	@echo "=== Install ==="
	@echo "Installing C wrapper to /usr/bin/freebuff..."
	@install -m 755 "$(STAGED)/bin/freebuff" "/data/data/com.termux/files/usr/bin/freebuff"
	@echo "Installing hook.so..."
	@mkdir -p "/data/data/com.termux/files/usr/lib/freebuff"
	@install -m 644 "$(STAGED)/lib/freebuff/hook.so" "/data/data/com.termux/files/usr/lib/freebuff/hook.so"
	@echo "Installing patches, scripts..."
	@cp -r "$(STAGED)/lib/freebuff/patches" "/data/data/com.termux/files/usr/lib/freebuff/patches"
	@cp -r "$(STAGED)/lib/freebuff/scripts" "/data/data/com.termux/files/usr/lib/freebuff/scripts"
	@echo ""
	@echo "[✓] Installed!"
	@echo "    Run: freebuff"

# ═══════════════════════════════════════════════════════════════════
test:
	@echo "=== Test ==="
	@bash "$(SCRIPTS_DIR)/test.sh"

# ═══════════════════════════════════════════════════════════════════
clean:
	@echo "Cleaning..."
	@rm -rf "$(ARTIFACTS)"
	@rm -f "$(WRAPPER_OUT)"
	@rm -f "$(HOOK_OUT)"
	@echo "  Removed: $(ARTIFACTS)"
	@echo "  Removed: $(WRAPPER_OUT)"
	@echo "  Removed: $(HOOK_OUT)"
	@echo "[✓] Clean"

# ═══════════════════════════════════════════════════════════════════
dev: produce stage test
	@echo ""
	@echo "[✓] dev complete"
