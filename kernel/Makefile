# SPDX-License-Identifier: GPL-3.0-or-later

ifneq ($(KERNELRELEASE),)
obj-m += infiltratorfs.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
