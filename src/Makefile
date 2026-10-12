# metastreamer. Native build: make. Cross build: make CC=arm-linux-gnueabihf-gcc (Buildroot passes its own).
# tools/build-zig.sh makes a static ARMv6 binary for a Pi Zero without any SDK.
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -std=gnu11
LDLIBS += -lpthread
OBJS = metastreamer.o hls.o hdmi.o telem.o wifi.o conf.o

all: metastreamer

metastreamer: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): $(wildcard *.h)
hls.o: settings_html.h

# the settings page is compiled into the binary
settings_html.h: web/settings.html gen_html.py
	python3 gen_html.py web/settings.html SETTINGS_HTML $@

clean:
	rm -f $(OBJS) metastreamer settings_html.h

.PHONY: all clean
