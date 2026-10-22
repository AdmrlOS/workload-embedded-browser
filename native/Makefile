CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 json-glib-1.0 libdrm)
LDLIBS += $(shell $(PKG_CONFIG) --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 json-glib-1.0 libdrm)
PLUGIN_DIR ?= /usr/local/lib/gstreamer-1.0

.PHONY: all install clean
all: libgstadmiralholepunch.so

SOURCES = gst-admiral-holepunch.c gst-admiral-videoplane.c

libgstadmiralholepunch.so: $(SOURCES) gst-admiral-videoplane.h drm-broker.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -fPIC -shared -o $@ $(SOURCES) $(LDLIBS)

install: all
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -m 0755 libgstadmiralholepunch.so $(DESTDIR)$(PLUGIN_DIR)/

clean:
	rm -f libgstadmiralholepunch.so
