CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
PACKAGES = wpe-webkit-2.0 wpebackend-fdo-1.0 wayland-server egl glesv2 gbm libdrm gio-unix-2.0 json-glib-1.0 libinput libudev xkbcommon
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))
PREFIX ?= /usr/local

.PHONY: all install clean
all: embedded-browser embedded-browser-bwrap

embedded-browser: embedded-browser.c input.c input.h ../native/drm-broker.c ../native/drm-broker.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -o $@ embedded-browser.c input.c ../native/drm-broker.c $(LDLIBS)

embedded-browser-bwrap: embedded-browser-bwrap.c
	$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -o $@ embedded-browser-bwrap.c

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 embedded-browser $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/libexec/embedded-browser
	install -m 0755 embedded-browser-bwrap $(DESTDIR)$(PREFIX)/libexec/embedded-browser/

clean:
	rm -f embedded-browser embedded-browser-bwrap
