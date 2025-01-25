# Variables
CC=$(CROSS_COMPILE)gcc
CFLAGS=-Wall -Wextra -g
TARGET=writer
SRCS=writer.c
OBJS=$(SRCS:.c=.o)

# Default target
.PHONY: default
default: $(TARGET)

# Build the target
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Compile .c files to .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJS)

