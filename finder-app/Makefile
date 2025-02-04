CC = aarch64-none-linux-gnu-gcc  # Use ARM64 cross-compiler

CFLAGS = -Wall -Wextra -g
TARGET = writer

SRCS = writer.c
OBJS = $(SRCS:.c=.o)

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJS)

