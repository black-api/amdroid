# Thimble engine - dependency-free C11 inference for ThimbleLM GGUFs.
#
#   make                 build/thimble (CLI + HTTP server), build/libthimble.a, build/libthimble.so
#   make OPENMP=1        parallel matmuls
#   make NATIVE=0        portable build (no -march=native)
#   make clean

CC      ?= cc
NATIVE  ?= 1
OPENMP  ?= 0

CFLAGS  ?= -O3
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -fPIC -D_DEFAULT_SOURCE -Iinclude -Isrc
LDLIBS  += -lm

ifeq ($(NATIVE),1)
CFLAGS  += -march=native
endif
ifeq ($(OPENMP),1)
CFLAGS  += -fopenmp
LDFLAGS += -fopenmp
endif

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/obj/%.o)
HDR := $(wildcard src/*.h include/*.h)

all: build/thimble build/thimble-stt build/libthimble.a build/libthimble.so

build/obj/%.o: src/%.c $(HDR) | build/obj
	$(CC) $(CFLAGS) -c $< -o $@

build/libthimble.a: $(OBJ)
	ar rcs $@ $^

build/libthimble.so: $(OBJ)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS)

TOOLS_SRC := $(wildcard tools/*.c)
TOOLS_HDR := $(wildcard tools/*.h)

build/thimble: $(TOOLS_SRC) $(TOOLS_HDR) build/libthimble.a
	$(CC) $(CFLAGS) -pthread -Itools $(LDFLAGS) -o $@ $(TOOLS_SRC) build/libthimble.a $(LDLIBS)

build/obj:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean

# speech to text: build/thimble-stt MODEL.thim FILE.wav
build/thimble-stt: stt/thimble_stt.c build/libthimble.a include/thimble_stt.h
	$(CC) $(CFLAGS) -o $@ stt/thimble_stt.c build/libthimble.a $(LDFLAGS) $(LDLIBS)
