obj-m += ls037v7dw06.o
SRC := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install
 
clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) clean
