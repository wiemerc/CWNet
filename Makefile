CC      := /opt/m68k-amigaos/bin/m68k-amigaos-gcc
CFLAGS  := -Wall

.PHONY: all clean

all: serecho logtest unmount listq cwnet-handler

clean:
	rm -f *.o serecho logtest unmount listq cwnet-handler

serecho: serecho.o
	$(CC) -noixemul -s -o $@ $@.o

logtest: logtest.o
	$(CC) -noixemul -s -o $@ $@.o

unmount: unmount.o
	$(CC) -noixemul -s -o $@ $@.o

listq: listq.o
	$(CC) -noixemul -s -o $@ $@.o

cwnet-handler: cwcrt0.o handler.o util.o netio.o
	$(CC) -L/opt/m68k-amigaos//m68k-amigaos/libnix/lib -L/opt/m68k-amigaos//m68k-amigaos/libnix/lib/libnix -s -o $@ $^ -lamiga -lnix -lnix13

