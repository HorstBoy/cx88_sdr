# SPDX-License-Identifier: GPL-2.0
cx88_sdr-y := cx88_sdr_core.o cx88_sdr_v4l2.o

obj-m += cx88_sdr.o

KVERSION = $(shell uname -r)
CURR_PWD = $(shell pwd)

all: module

module:
	make -C /lib/modules/$(KVERSION)/build M=$(CURR_PWD) modules

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(CURR_PWD) clean
