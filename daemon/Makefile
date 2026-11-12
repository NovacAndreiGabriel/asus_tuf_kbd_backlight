.RECIPEPREFIX = >

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
BUILD  := build
BIN    := $(BUILD)/tuf_kbdctl

all: $(BIN)

$(BIN): tuf_kbdctl.c | $(BUILD)
> $(CC) $(CFLAGS) -o $@ $<

$(BUILD):
> mkdir -p $@

install: all
> install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/tuf_kbdctl
> install -Dm644 tuf_kbdd.service $(DESTDIR)/etc/systemd/system/tuf_kbdd.service
> install -Dm644 99-tuf_kbd.rules $(DESTDIR)/etc/udev/rules.d/99-tuf_kbd.rules

uninstall:
> rm -f $(DESTDIR)$(PREFIX)/bin/tuf_kbdctl
> rm -f $(DESTDIR)/etc/systemd/system/tuf_kbdd.service
> rm -f $(DESTDIR)/etc/udev/rules.d/99-tuf_kbd.rules

clean:
> rm -rf $(BUILD)

.PHONY: all install uninstall clean
