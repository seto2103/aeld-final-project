CC = $(CROSS_COMPILE)gcc

VERSION ?= 0.1.0

# Kept separate from CFLAGS/LDFLAGS so they survive Buildroot passing its own on the command line
APP_CFLAGS = -Wall -Wextra -pthread -DVERSION=\"$(VERSION)\"
APP_LDLIBS = -pthread

TARGET = camera-server

SRC = src/main.c src/capture.c src/frame_store.c src/http_server.c
HDR = src/capture.h src/frame_store.h src/http_server.h

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(APP_CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRC) $(APP_LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
