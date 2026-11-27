CC ?= cc
CFLAGS ?= -O3 -march=native
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra
LDLIBS = -lm -lpthread

all: bin/rvntsc

bin/rvntsc: src/rvntsc.c src/ntsc.c src/ntsc.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ src/rvntsc.c src/ntsc.c $(LDLIBS)

test: bin/rvntsc
	python3 -m unittest discover -s tests -v

clean:
	rm -rf bin

.PHONY: all test clean
