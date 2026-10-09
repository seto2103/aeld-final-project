CC = $(CROSS_COMPILE)gcc

VERSION ?= 0.1.0

# Kept separate from CFLAGS/LDFLAGS so they survive Buildroot passing its own on the command line
# ../driver provides status_led_ioctl.h on host builds; Buildroot builds get it from staging
APP_CFLAGS = -Wall -Wextra -pthread -DVERSION=\"$(VERSION)\" -I../driver
APP_LDLIBS = -pthread -ljpeg

TARGETS = camera-server camera-supervisor

SRC = src/main.c src/capture.c src/frame_store.c src/http_server.c src/motion.c src/frame_ring.c src/avi_writer.c src/recorder.c src/led.c
HDR = src/capture.h src/frame_store.h src/http_server.h src/motion.h src/thread_util.h src/frame_ring.h src/avi_writer.h src/recorder.h src/led.h

all: $(TARGETS)

camera-server: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(APP_CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(APP_LDLIBS)

# Single threaded, and needs no libraries
camera-supervisor: src/supervisor.c
	$(CC) $(CFLAGS) $(APP_CFLAGS) $(LDFLAGS) -o $@ src/supervisor.c

clean:
	rm -f $(TARGETS)

.PHONY: all clean
