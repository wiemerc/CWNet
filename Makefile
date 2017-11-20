CC      := /opt/m68k-amigaos/bin/m68k-amigaos-gcc
CFLAGS  := -Wall
LDFLAGS := -noixemul

.PHONY: all clean

all: serecho

clean:
	rm -f *.o serecho

serecho: serecho.o

