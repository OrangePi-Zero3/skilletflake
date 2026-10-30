# SPDX-License-Identifier: MIT
# Host unit tests for kmd/core: builds the OS-neutral core against pw_os_mock.c.
#   make            build + run all tests (ASan + UBSan, -Werror)
#   make build      build only
#   make CC=clang   use clang instead of gcc
#   ./pw_tests mmu  run tests whose name contains "mmu"
#   PW_FW_PATH=/path/to/mali_csffw.bin ./pw_tests   (default: ../../../references/firmware/)
#   PW_TEST_VERBOSE=1 ./pw_tests                     print core log lines
ifeq ($(origin CC),default)
CC      := gcc
endif
REPO    := ../..
CORE    := $(REPO)/kmd/core
CFLAGS  ?= -O1 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -fno-omit-frame-pointer \
           -fsanitize=address,undefined -fno-sanitize-recover=undefined \
           -I$(CORE) -I$(REPO)/include -I$(REPO)/include/drm-uapi -I. -pthread
LDFLAGS += -fsanitize=address,undefined -pthread

CORE_SRCS := $(addprefix $(CORE)/,$(shell sed -n "s/^  '\(pw_[a-z_]*\.c\)',/\1/p" $(CORE)/meson.build))
TEST_SRCS := test_main.c test_util.c test_fw_parse.c test_mmu.c test_device.c test_mem.c test_sync.c test_sched.c pw_os_mock.c
OBJDIR    := obj
OBJS      := $(addprefix $(OBJDIR)/core_,$(notdir $(CORE_SRCS:.c=.o))) \
             $(addprefix $(OBJDIR)/,$(TEST_SRCS:.c=.o))
HDRS      := $(wildcard $(CORE)/*.h) $(wildcard *.h)

all: test

build: pw_tests

pw_tests: $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJDIR)/core_%.o: $(CORE)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

$(OBJDIR):
	mkdir -p $@

test: pw_tests
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./pw_tests

# Freestanding ARM64 Windows-kernel compile check of every core source (no link).
LLVM_MINGW ?= /opt/llvm-mingw
KCC := $(LLVM_MINGW)/bin/aarch64-w64-mingw32-clang
KCFLAGS := -c -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector \
           -funwind-tables -I$(CORE) -I$(REPO)/include -I$(REPO)/include/drm-uapi \
           -I$(REPO)/include/compat -I$(REPO)/kmd/compat-km
freestanding: | $(OBJDIR)
	@for f in $(CORE_SRCS); do \
		echo "  KCC $$f"; \
		$(KCC) $(KCFLAGS) $$f -o $(OBJDIR)/arm64_$$(basename $$f .c).o || exit 1; \
	done
	@echo "undefined symbols (must be pw_os_* only, plus possibly memcpy/memset):"
	@$(LLVM_MINGW)/bin/llvm-nm -u $(OBJDIR)/arm64_*.o | awk 'NF==2 {print $$2}' | sort -u | \
		grep -v '^pw_\(gpu\|mmu\|fw\|irq\|vm\|poll\|mem\|str\|snprintf\|vsnprintf\|exception\|device\)' | tr '\n' ' '; echo

clean:
	rm -rf $(OBJDIR) pw_tests

.PHONY: all build test clean freestanding
