# SPDX-License-Identifier: GPL-3.0-or-later

# Keep the Kbuild declaration unconditional. DKMS exports KERNELRELEASE before
# invoking this Makefile; if all normal targets are hidden behind a
# KERNELRELEASE conditional, the initial DKMS make terminates with
# "make: *** No targets. Stop." before Kbuild is ever entered.
obj-m += infiltratorfs.o
ccflags-y += -I$(src)/../include/infilfs

KDIR ?= /lib/modules/$(shell uname -r)/build

# Module BTF can only be generated when the matching kernel's base BTF image is
# available. Distribution header packages commonly omit vmlinux, in which case
# Kbuild otherwise compiles the module successfully and then emits a misleading
# "Skipping BTF generation" message. Disable that optional stage only for such
# header trees; when vmlinux (or the newer vmlinux.unstripped) is present, keep
# the kernel's normal CONFIG_DEBUG_INFO_BTF_MODULES behaviour intact.
ifeq ($(strip $(wildcard $(KDIR)/vmlinux $(KDIR)/vmlinux.unstripped)),)
INFILTRATORFS_BTF_OVERRIDE := CONFIG_DEBUG_INFO_BTF_MODULES=
endif

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(INFILTRATORFS_BTF_OVERRIDE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean