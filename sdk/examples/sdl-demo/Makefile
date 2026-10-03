# An SDL 2 program as a Vexa app: `make` builds build/SDLDemo.vxapp.
# (Like the app template, with SDL: sdl2-config gives its flags.)
APP := SDLDemo
EXECUTABLE := sdl-demo
SDK ?= ../..

CC := $(SDK)/bin/vexa-cc
CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += $(shell $(SDK)/bin/sdl2-config --cflags)
LDLIBS += $(shell $(SDK)/bin/sdl2-config --libs) -lm

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=build/obj/%.o)
BUNDLE := build/$(APP).vxapp

all: $(BUNDLE)

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build/$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUNDLE): build/$(EXECUTABLE) Info.conf $(wildcard Resources/*)
	rm -rf $@
	mkdir -p $@/Contents/Vexa $@/Contents/Resources
	cp Info.conf $@/Contents/
	cp build/$(EXECUTABLE) $@/Contents/Vexa/
	cp -R Resources/. $@/Contents/Resources/

clean:
	rm -rf build

.PHONY: all clean
-include $(OBJECTS:.o=.d)
