NAME = AX88179
ICON = icon.png
DESCRIPTION = "AX88179 USB Driver"
COMPRESSED = YES
COMPRESSED_MODE = zx0

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

SOURCES = main.c
ARCHIVED = YES
BSSHEAP_LOW ?= 0xD072C6

include $(shell cedev-config --makefile)
