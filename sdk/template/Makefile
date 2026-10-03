# A Vexa app. `make` builds build/Hello.vxapp: copy it to Vexa (into /apps,
# or anywhere) and double-click it in Files, or start it with
# `open Hello.vxapp`.
#
# Change APP and EXECUTABLE (and Info.conf) to rename it.
APP := Hello
EXECUTABLE := hello

# Where the Vexa SDK is (vexa-new-app writes it here).
SDK ?= ..

CC := $(SDK)/bin/vexa-cc
CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS ?=

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=build/obj/%.o)
BUNDLE := build/$(APP).vxapp

all: $(BUNDLE)

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build/$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# The bundle: Info.conf, the program in Contents/Vexa, and Resources.
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
