CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g
LIBS = -lncurses
TARGET = monitor

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $(TARGET).c -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
