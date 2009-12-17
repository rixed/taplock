CFLAGS  =-W -Wall -std=c99 -D_GNU_SOURCE
LDFLAGS =-lX11

all: taplock

taplock: taplock.o topmost.o

.PHONY: clean

clean:
	rm -f *.o

PREFIX := /usr/local/

install: taplock
	install -d $(DESTDIR)$(PREFIX)bin/
	install ./taplock $(DESTDIR)$(PREFIX)bin/
