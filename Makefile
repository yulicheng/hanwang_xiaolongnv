obj-m := hwln.o

KERNELDIR ?= /usr/src/linux-headers-$(shell uname -r)
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
	rm -rf *.o *.ko *.mod.c .*.cmd modules.order Module.symvers .tmp_versions

