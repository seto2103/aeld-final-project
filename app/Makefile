CC = $(CROSS_COMPILE)gcc

VERSION ?= 0.1.0

# Kept separate from CFLAGS/LDFLAGS so they survive Buildroot passing its own on the command line
APP_CFLAGS = -Wall -Wextra -pthread -DVERSION=\"$(VERSION)\"
APP_LDLIBS = -pthread

TARGET = camera-server

SRC = src/main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(APP_CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRC) $(APP_LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
