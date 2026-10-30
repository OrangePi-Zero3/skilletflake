# SPDX-License-Identifier: MIT
# Host tests of the kmd/win glue: pw_win_ioctl.c + pw_os_win.c + pw_win_user.c + pw_fmt.c run
# against the REAL kmd/core on a pthread fake of the NT/KMDF API (wdk_shim/, fake_wdk.c).
#   make              build + run (ASan + UBSan, -Werror)
#   make CC=clang     same with clang
#   make clean && make SAN=thread CC=clang   ThreadSanitizer instead of ASan+UBSan
#                     (gcc refuses the core's atomic fences under TSan)
#   ./test_glue mmap  run tests whose name contains "mmap"
#   ./test_plat       board identification from ACPI hardware IDs (pw_win_plat.c)
#   ./test_stub       the -Dcore=stub core (pw_core_stub.c) behind the glue, G31 + G610 GPU_IDs
#   PW_TEST_VERBOSE=1 ./test_glue   print driver log lines
# These test the glue's logic (concurrency, teardown, bookkeeping), not Windows itself.
ifeq ($(origin CC),default)
CC      := gcc
endif
REPO    := ../../..
CORE    := $(REPO)/kmd/core
WIN     := ..
CFLAGS  ?= -O1 -g
SAN     ?= address,undefined
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -fno-omit-frame-pointer \
           -fsanitize=$(SAN) -fno-sanitize-recover=all -pthread
LDFLAGS += -fsanitize=$(SAN) -pthread
ifeq ($(SAN),thread)
CFLAGS  += -DFAKE_NO_SEH_FAULTS=1
endif
CORE_INC := -I$(CORE) -I$(REPO)/include -I$(REPO)/include/drm-uapi
WIN_INC  := -Wno-multichar -Iwdk_shim -I$(WIN) -I. $(CORE_INC)

CORE_SRCS := $(addprefix $(CORE)/,$(shell sed -n "s/^  '\(pw_[a-z_]*\.c\)',/\1/p" $(CORE)/meson.build))
WIN_SRCS  := pw_os_win.c pw_win_ioctl.c pw_win_user.c pw_fmt.c
OBJDIR    := obj
OBJS      := $(addprefix $(OBJDIR)/core_,$(notdir $(CORE_SRCS:.c=.o))) \
             $(addprefix $(OBJDIR)/win_,$(WIN_SRCS:.c=.o)) \
             $(OBJDIR)/fake_wdk.o $(OBJDIR)/test_glue.o
HDRS      := $(wildcard $(CORE)/*.h) $(wildcard $(WIN)/*.h) $(wildcard *.h) $(wildcard wdk_shim/*.h)

# pw_os_win.c: the AArch64 inline asm is neutered by the shim, leaving CTR_EL0 unread.
# pw_win_user.c: __try/__except are emulated with setjmp/longjmp.
WARN_pw_os_win.c   := -Wno-uninitialized -Wno-maybe-uninitialized -Wno-unknown-warning-option
WARN_pw_win_user.c := -Wno-clobbered -Wno-unknown-warning-option

all: test

build: test_glue test_plat test_stub

test_glue: $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# The -Dcore=stub core (pw_core_stub.c) behind the same glue: the Z1 bring-up build.
STUB_OBJS := $(addprefix $(OBJDIR)/win_,$(WIN_SRCS:.c=.o)) $(OBJDIR)/win_pw_core_stub.o \
             $(OBJDIR)/fake_wdk.o $(OBJDIR)/test_stub.o

test_stub: $(STUB_OBJS)
	$(CC) $(STUB_OBJS) -o $@ $(LDFLAGS)

# Board identification from ACPI hardware IDs (pw_win_plat.c has no OS dependencies).
test_plat: test_plat.c $(WIN)/pw_win_plat.c $(WIN)/pw_win_plat.h
	$(CC) $(CFLAGS) -std=c11 -I$(WIN) test_plat.c $(WIN)/pw_win_plat.c -o $@ $(LDFLAGS)

$(OBJDIR)/core_%.o: $(CORE)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) -std=c11 $(CORE_INC) -c $< -o $@

$(OBJDIR)/win_%.o: $(WIN)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) $(WIN_INC) $(WARN_$(notdir $<)) -c $< -o $@

$(OBJDIR)/%.o: %.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) $(WIN_INC) -c $< -o $@

$(OBJDIR):
	mkdir -p $@

test: test_glue test_plat test_stub
	./test_plat
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./test_stub
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./test_glue

clean:
	rm -rf $(OBJDIR) test_glue test_plat test_stub

.PHONY: all build test clean
