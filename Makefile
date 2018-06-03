CC      := /opt/m68k-amigaos/bin/m68k-amigaos-gcc
CFLAGS  := -Wall

.PHONY: all clean

all: serecho logtest cwnet-handler

clean:
	rm -f *.o serecho logtest cwnet-handler

serecho: serecho.o
	$(CC) -noixemul -o $@ $@.o

logtest: logtest.o
	$(CC) -noixemul -o $@ $@.o

cwnet-handler: cwcrt0.o handler.o
	$(CC) -L/opt/m68k-amigaos//m68k-amigaos/libnix/lib -L/opt/m68k-amigaos//m68k-amigaos/libnix/lib/libnix -s -o $@ $^ -lamiga -lnix

