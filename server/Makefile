CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11

TARGET = monitor_server
SRC = monitor_server.c event_loop.c client.c protocol.c telemetry.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -I../include -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)