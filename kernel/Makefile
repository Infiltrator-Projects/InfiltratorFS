# SPDX-License-Identifier: GPL-3.0-or-later

# Keep the Kbuild declaration unconditional. DKMS exports KERNELRELEASE before
# invoking this Makefile; if all normal targets are hidden behind a
# KERNELRELEASE conditional, the initial DKMS make terminates with
# "make: *** No targets. Stop." before Kbuild is ever entered.
obj-m += infiltratorfs.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
