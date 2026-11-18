# SPDX-License-Identifier: GPL-2.0-only
.DEFAULT_GOAL := all
BUILD := build
JPEG := $(BUILD)/jpeg-9f
APP := ../../src/canoroid
include jpeg.mk
JPEG_OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(CR_JPEG_NAMES)))
CFLAGS ?= -O2 -g

.PHONY: all clean
all: $(BUILD)/canoroid-print

$(JPEG)/jconfig.h: fetch-jpeg.sh
	sh fetch-jpeg.sh $(BUILD)

$(BUILD)/%.o: $(JPEG)/jconfig.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DNO_GETENV -I$(JPEG) -c $(JPEG)/$*.c -o $@

$(BUILD)/canoroid-print: host.c $(APP)/print.c $(APP)/print.h $(JPEG_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror \
		-I$(APP) -I$(JPEG) host.c $(APP)/print.c $(JPEG_OBJECTS) -o $@

clean:
	rm -rf $(BUILD)
