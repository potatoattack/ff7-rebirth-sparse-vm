# SPDX-License-Identifier: MIT
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Werror
VULKAN_CFLAGS := $(shell $(PKG_CONFIG) --cflags vulkan)
VULKAN_LIBS := $(shell $(PKG_CONFIG) --libs vulkan)

.PHONY: all self-test clean
all: build/lifetimes build/neighbors

build:
	mkdir -p $@

build/lifetimes: native/ff7_destroy_native.c native/e16_regression.inc native/e09_native_base.c native/e12_native_base.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(VULKAN_CFLAGS) -pthread $< $(LDFLAGS) $(VULKAN_LIBS) -o $@

build/neighbors: native/e22_neighbor.c native/e22_native_support.c native/e09_native_base.c native/e12_native_base.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(VULKAN_CFLAGS) -pthread $< $(LDFLAGS) $(VULKAN_LIBS) -o $@

self-test: all
	./build/lifetimes --self-test
	./build/neighbors --self-test

clean:
	rm -rf build
