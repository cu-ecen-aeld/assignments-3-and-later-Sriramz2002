CC = gcc
CFLAGS = -Wall -Wextra -g
TARGET = aesdsocket

.PHONY: all clean

all: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.c

clean:
	rm -f $(TARGET)

