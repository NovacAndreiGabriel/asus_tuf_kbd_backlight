obj-m := tuf_kbd.o

KDIR  ?= /lib/modules/$(shell uname -r)/build
BUILD := $(CURDIR)/build

all:
	@mkdir -p $(BUILD)
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(BUILD) modules

clean:
	rm -rf $(BUILD)

.PHONY: all clean
