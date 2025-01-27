
CC=$(CROSS_COMPILE)gcc

CFLAG=-Wall -Wextra -g
TARGET=writer

SRCS=writer.c

OBJS=$(SRCS:.c=.o)

#Default target

.PHONY: default
default: $(TARGET)

# Bild the target

$(TARGET): $(OBJS)
	$(CC) $(CFLAG) -o $(TARGET) $(OBJS)

# Compile .c files to .o files
%.o: %.c
	$(CC) $(CFLAG) -c $< -o $@

# Clean up

.PHONY: clean
clean:
	rm -f $(TARGET) $(OBJS)

