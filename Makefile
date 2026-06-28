CC = gcc
CFLAGS = -Wall -Iinclude -pthread

all: server client

server: src/server.c src/utils.c
	$(CC) $(CFLAGS) src/server.c src/utils.c -o server

client: src/client.c src/utils.c
	$(CC) $(CFLAGS) src/client.c src/utils.c -o client

clean:
	rm -f server client
