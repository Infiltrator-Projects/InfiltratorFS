# SPDX-License-Identifier: GPL-3.0-or-later
# Freestanding Common V2 module; run from this directory or via make -C module.
CC ?= cc
ROOT := ..
OUT ?= $(ROOT)/build-v2
COMMON := $(ROOT)/src/infiltratr-common
# Exact V2 header from Libraries commit b50f493d789b840edea40ea09978d94d751533f8.
INCLUDES := -Icommon -I$(ROOT)/include -I$(COMMON)/include -I$(ROOT)/uefi/filesystems/lz4
BASE := -std=c11 -O2 -fPIC -fvisibility=hidden -ffreestanding -fno-builtin \
        -fno-stack-protector -fno-tree-loop-distribute-patterns -fno-tree-vectorize \
        -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -mno-red-zone \
        -ffunction-sections -fdata-sections -Wall -Wextra
EFI := $(BASE) -mgeneral-regs-only -DINFS_COMMON_V2_FIRMWARE -DINFS_FREESTANDING \
       -DLZ4_FREESTANDING=1 -DLZ4_memcpy=memcpy -DLZ4_memset=memset \
       -DLZ4_memmove=memmove -include $(ROOT)/uefi/filesystems/platform/uefi_libc.h
SOURCES := common_v2.c freestanding.c $(ROOT)/src/checksum.c $(ROOT)/src/locator.c \
           $(ROOT)/src/status.c $(ROOT)/src/storage.c $(ROOT)/src/security.c \
           $(ROOT)/src/fs.c $(ROOT)/src/volume.c $(COMMON)/src/arithmetic.c \
           $(ROOT)/uefi/filesystems/lz4/lz4.c
.PHONY: all clean verify-common
all: verify-common $(OUT)/InfiltratorFS.so
verify-common:
	@printf '%s  %s\n' 692fa7bc005d855b5268dd66b952a73b76e084027ef13cf992e205a33c6930bd common/infiltratr/filesystem_module_v2.h | sha256sum -c - >/dev/null
$(OUT):
	mkdir -p $@
# Unused floating-point helpers are discarded by --gc-sections; verify the
# linked firmware image has no vector/floating-point instructions.
$(OUT)/core.o: $(COMMON)/src/core.c | $(OUT)
	$(CC) $(BASE) $(INCLUDES) -c $< -o $@
$(OUT)/InfiltratorFS.so: $(SOURCES) $(wildcard $(ROOT)/src/volume/*.inc) $(OUT)/core.o exports.map common/infiltratr/filesystem_module_v2.h
	$(CC) $(EFI) $(INCLUDES) -nostdlib -shared \
	  -Wl,--gc-sections,-z,defs,--version-script=exports.map \
	  $(SOURCES) $(OUT)/core.o -o $@
clean:
	@test "$(OUT)" = "$(ROOT)/build-v2" || { echo 'Refusing to clean a custom output path' >&2; exit 1; }
	rm -rf -- "$(ROOT)/build-v2"
