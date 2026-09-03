CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11

TARGET = monitor_cli
SRC = monitor_cli.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean