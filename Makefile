CC=gcc
CFLAGS=-std=c99 -w -lm -lpthread

EXE=tgen peer tracker

all: ${EXE}

${EXE}:
	$(CC) $@.c -o $@ $(CFLAGS)

clean:
	rm -rf *o ${EXE}
