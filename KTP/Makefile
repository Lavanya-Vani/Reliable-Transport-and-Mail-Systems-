# Makefile - KTP Mini Project 1
CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2
LDFLAGS = -lpthread

.PHONY: all clean

all: libksocket.a initksocket user1 user2

ksocket.o: ksocket.c ksocket.h
	$(CC) $(CFLAGS) -c ksocket.c -o ksocket.o

libksocket.a: ksocket.o
	ar rcs libksocket.a ksocket.o

initksocket: initksocket.c ksocket.h libksocket.a
	$(CC) $(CFLAGS) initksocket.c -o initksocket -L. -lksocket $(LDFLAGS)

user1: user1.c ksocket.h libksocket.a
	$(CC) $(CFLAGS) user1.c -o user1 -L. -lksocket $(LDFLAGS)

user2: user2.c ksocket.h libksocket.a
	$(CC) $(CFLAGS) user2.c -o user2 -L. -lksocket $(LDFLAGS)

clean:
	rm -f *.o libksocket.a initksocket user1 user2 received_output.txt
