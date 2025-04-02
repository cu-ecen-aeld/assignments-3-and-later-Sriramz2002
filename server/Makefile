# Force AArch64 none-linux-gnu cross-compiler
CROSS_COMPILE := aarch64-none-linux-gnu-

CC := $(CROSS_COMPILE)gcc

CFLAGS := -Wall -Wextra -g

LDFLAGS := -lpthread -lrt

TARGET := aesdsocket
SRCS := aesdsocket.c
OBJS := $(SRCS:.c=.o)

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJS)

