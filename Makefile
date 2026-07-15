CC ?= cc
CFLAGS ?= -O2 -Wall
LDFLAGS ?= -lpthread

all: sender receiver

sender: sender.c
	$(CC) $(CFLAGS) -o sender sender.c $(LDFLAGS)

receiver: receiver.c
	$(CC) $(CFLAGS) -o receiver receiver.c $(LDFLAGS)

clean:
	rm -f sender receiver
