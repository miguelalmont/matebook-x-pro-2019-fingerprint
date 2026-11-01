# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build of the GXFP51B7 transport module.
#   make        builds gxfp51b7.ko for the running kernel
#   make clean
# kbuild does not support spaces in this directory's path.

obj-m += gxfp51b7.o

ccflags-y := -I$(src)/include

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
