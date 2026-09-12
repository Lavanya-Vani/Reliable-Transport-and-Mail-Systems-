CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -g

all: smserver smclient

smserver: smserver.c
	$(CC) $(CFLAGS) -o smserver smserver.c

smclient: smclient.c
	$(CC) $(CFLAGS) -o smclient smclient.c

clean:
	rm -f smserver smclient

.PHONY: all clean
