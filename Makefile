# X-Streemly — libretro network-video core
#
#   make              build deps (first time) + core
#   make DEBUG=1      -O0 -g core build
#   make check        harness tests against local fixtures over a local
#                     HTTP(S) server (tests/server.py); no internet needed
#   make fixtures     generate the test streams (needs ffmpeg + openssl)
#
# Linux x86_64 only until M5 (CLAUDE.md D-015); other platforms follow
# DeeVee's Makefile.

.DEFAULT_GOAL := all

TARGET_NAME := xstreemly
DEBUG       ?= 0

UNAME := $(shell uname -s)
platform ?= unix

ifeq ($(platform),unix)
  TARGET    := $(TARGET_NAME)_libretro.so
  SHARED    := -shared -Wl,--version-script=link.T -Wl,--no-undefined
  BUILD_DIR ?= build
  HOST_LIBS := -ldl -lpthread
else
  $(error unknown platform '$(platform)' (only 'unix' until M5))
endif

include Makefile.deps
include Makefile.common

PKG_CONFIG ?= pkg-config
PKGS       := libavformat libavfilter libavcodec libswscale libswresample libavutil
DEPS_PKG_CONFIG_PATH := $(FFMPEG_PREFIX)/lib/pkgconfig
DEPS_ENV   := PKG_CONFIG_PATH=$(DEPS_PKG_CONFIG_PATH) PKG_CONFIG_LIBDIR=$(DEPS_PKG_CONFIG_PATH)
# Evaluated lazily: the .pc files only exist once deps are built.
DEPS_CFLAGS = $(shell $(DEPS_ENV) $(PKG_CONFIG) --cflags $(PKGS))
DEPS_LIBS   = $(shell $(DEPS_ENV) $(PKG_CONFIG) --static --libs $(PKGS))

ifeq ($(DEBUG),1)
  OPTFLAGS := -O0 -g
else
  OPTFLAGS := -O2 -g
endif

CFLAGS   += -std=c99 -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden $(OPTFLAGS)
CPPFLAGS += $(INCFLAGS) -D_POSIX_C_SOURCE=200809L
LDFLAGS  += $(SHARED)
LIBS     := -L$(MBEDTLS_PREFIX)/lib -lm -lpthread

OBJ_DIR := $(BUILD_DIR)/obj
OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES_C))

all: $(TARGET)

$(TARGET): $(DEPS_STAMPS) $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(DEPS_LIBS) $(LIBS)

# Objects depend on the deps being installed (headers). Order-only so a dep
# rebuild doesn't force a full core rebuild; -MMD tracks header changes.
$(OBJ_DIR)/%.o: %.c | $(DEPS_STAMPS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(DEPS_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

distclean: clean
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)

.PHONY: all clean distclean

# ---- tests -----------------------------------------------------------------
HARNESS     := $(BUILD_DIR)/harness
FIXTURE_DIR := build/fixtures

$(HARNESS): tests/harness.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -Wall -Wextra -Wno-unused-parameter -O1 -g $(INCFLAGS) -o $@ $< $(HOST_LIBS) -lm

harness: $(HARNESS)

FIXTURE_STAMP := $(FIXTURE_DIR)/.stamp

$(FIXTURE_STAMP): tests/fixtures/make_fixtures.sh
	tests/fixtures/make_fixtures.sh $(FIXTURE_DIR)
	@touch $@

fixtures: $(FIXTURE_STAMP)

check: $(TARGET) $(HARNESS) $(FIXTURE_STAMP)
	@sh tests/check.sh ./$(TARGET) $(HARNESS) $(FIXTURE_DIR) $(BUILD_DIR)

.PHONY: harness fixtures check
