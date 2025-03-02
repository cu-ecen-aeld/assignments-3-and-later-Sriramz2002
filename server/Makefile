CC = gcc 

CFLAGS = -Wall -Wextra -g -pthread
TARGET = aesdsocket

SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

.PHONY: all
all: $(TARGET)

LDFLAGS = -pthread
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJS)

