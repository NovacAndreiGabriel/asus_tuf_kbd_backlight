.RECIPEPREFIX = >

all: driver daemon

driver:
> $(MAKE) -C driver

daemon:
> $(MAKE) -C daemon

install: daemon
> $(MAKE) -C daemon install

dkms:
> -dkms remove tuf_kbd/0.1 --all
> rm -rf /usr/src/tuf_kbd-0.1
> install -Dm644 -t /usr/src/tuf_kbd-0.1 driver/tuf_kbd.c driver/Makefile driver/dkms.conf
> dkms install tuf_kbd/0.1

clean:
> $(MAKE) -C driver clean
> $(MAKE) -C daemon clean

.PHONY: all driver daemon install dkms clean
