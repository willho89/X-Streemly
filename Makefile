# X-Streemly — libretro network-video core
#
#   make              build deps (first time) + core
#   make DEBUG=1      -O0 -g core build
#   make check        harness tests against local fixtures over a local
#                     HTTP(S) server (tests/server.py); no internet needed
#   make fixtures     generate the test streams (needs ffmpeg + openssl)
#   make platform=win          Windows x86_64 DLL; on Linux cross-compiles with
#                              MinGW-w64 (CROSS=x86_64-w64-mingw32-), TLS by
#                              schannel, tests run under Wine; build/win64
#   make platform=osx          macOS .dylib (native build on a Mac)
#   make platform=linux-arm64  Linux aarch64 / armhf .so, cross-compiled
#   make platform=linux-armhf  (aarch64-linux-gnu- / arm-linux-gnueabihf-),
#                              tests run under qemu-user
#   make platform=android-arm64  Android arm64-v8a / armeabi-v7a
#   make platform=android-armv7  (NDK=~/android-ndk/android-ndk-r27c, API 21);
#                              build + link only
#
# Platforms follow DeeVee's Makefile (CLAUDE.md §6, M5).

.DEFAULT_GOAL := all

TARGET_NAME := xstreemly
DEBUG       ?= 0

UNAME := $(shell uname -s)
ifeq ($(OS),Windows_NT)
  platform ?= win
else ifeq ($(UNAME),Darwin)
  platform ?= osx
else
  platform ?= unix
endif

ifeq ($(platform),unix)
  TARGET    := $(TARGET_NAME)_libretro.so
  SHARED    := -shared -Wl,--version-script=link.T -Wl,--no-undefined
  BUILD_DIR ?= build
  HOST_LIBS := -ldl -lpthread
  # Keep the oldest symbol versions (glibc 2.35 floor, src/xs_glibc_compat.c).
  EXTRA_SOURCES := src/xs_glibc_compat.c
else ifeq ($(platform),osx)
  # Native build on a Mac (Apple's SDK isn't licensed for other hosts); CI
  # builds arm64 and x86_64 on GitHub Actions macOS runners.
  TARGET    := $(TARGET_NAME)_libretro.dylib
  SHARED    := -dynamiclib -Wl,-exported_symbol,_retro_*
  BUILD_DIR ?= build/osx
  HOST_LIBS := -lpthread
  # _POSIX_C_SOURCE (below) hides the BSD types Apple's own headers use.
  CPPFLAGS  += -D_DARWIN_C_SOURCE
  MACOSX_DEPLOYMENT_TARGET ?= 11.0
  export MACOSX_DEPLOYMENT_TARGET
else ifeq ($(platform),win)
  TARGET    := $(TARGET_NAME)_libretro.dll
  # Fully static (libgcc, winpthreads): the DLL needs only Windows' own DLLs.
  SHARED    := -shared -static -static-libgcc -Wl,--version-script=link.T -Wl,--no-undefined
  BUILD_DIR ?= build/win64
  EXE       := .exe
  HOST_LIBS := -static -lpthread
  # Windows' own TLS (D-004): no mbedTLS, no system CA bundle needed.
  FFMPEG_TLS := --enable-schannel
  FFMPEG_THREADS := --enable-w32threads
  NO_MBEDTLS := 1
  ifneq ($(OS),Windows_NT)
    # Cross from Linux; the -posix toolchain has the pthreads rthreads uses.
    CROSS   ?= x86_64-w64-mingw32-
    CC      := $(CROSS)gcc-posix
    AR      := $(CROSS)ar
    RUN     ?= env WINEDEBUG=-all wine
    FFMPEG_TARGET := --enable-cross-compile --cross-prefix=$(CROSS) --cc=$(CC) \
                     --target-os=mingw32 --arch=x86_64
  endif
else ifneq ($(filter linux-arm64 linux-armhf,$(platform)),)
  BUILD_DIR ?= build/$(platform)
  # Same file name as the native core: keep it in the build dir so it never
  # replaces the host's xstreemly_libretro.so.
  TARGET    := $(BUILD_DIR)/$(TARGET_NAME)_libretro.so
  SHARED    := -shared -Wl,--version-script=link.T -Wl,--no-undefined
  HOST_LIBS := -ldl -lpthread
  ifeq ($(platform),linux-arm64)
    CROSS        ?= aarch64-linux-gnu-
    FFMPEG_ARCH  := aarch64
    RUN          ?= qemu-aarch64 -L /usr/aarch64-linux-gnu
  else
    CROSS        ?= arm-linux-gnueabihf-
    FFMPEG_ARCH  := arm
    RUN          ?= qemu-arm -L /usr/arm-linux-gnueabihf
  endif
  CC      := $(CROSS)gcc
  AR      := $(CROSS)ar
  EXTRA_SOURCES := src/xs_glibc_compat.c
  SHARED  += -Wl,--wrap=fmod
  FFMPEG_TARGET := --enable-cross-compile --cross-prefix=$(CROSS) --cc=$(CC) \
                   --target-os=linux --arch=$(FFMPEG_ARCH)
else ifneq ($(filter android-arm64 android-armv7,$(platform)),)
  NDK       ?= $(HOME)/android-ndk/android-ndk-r27c
  NDK_BIN   := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
  BUILD_DIR ?= build/$(platform)
  TARGET    := $(BUILD_DIR)/$(TARGET_NAME)_libretro_android.so
  # --as-needed: FFmpeg's configure always adds -landroid -lmediandk on
  # Android; nothing we build uses them.
  SHARED    := -shared -Wl,--version-script=link.T -Wl,--no-undefined -Wl,--as-needed
  ifeq ($(platform),android-arm64)
    CC          := $(NDK_BIN)/aarch64-linux-android21-clang
    FFMPEG_ARCH := --arch=aarch64
  else
    CC          := $(NDK_BIN)/armv7a-linux-androideabi21-clang
    FFMPEG_ARCH := --arch=arm --cpu=armv7-a
  endif
  AR      := $(NDK_BIN)/llvm-ar
  STRIP   := $(NDK_BIN)/llvm-strip
  # Software decoding only (MediaCodec/JNI would need a Java VM handle).
  FFMPEG_TARGET := --enable-cross-compile --target-os=android $(FFMPEG_ARCH) --cc=$(CC) \
                   --disable-mediacodec --disable-jni \
                   --ar=$(AR) --nm=$(NDK_BIN)/llvm-nm --ranlib=$(NDK_BIN)/llvm-ranlib \
                   --strip=$(STRIP)
else
  $(error unknown platform '$(platform)' (unix, osx, win, linux-arm64, linux-armhf, android-arm64, android-armv7))
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
LIBS     := $(if $(NO_MBEDTLS),,-L$(MBEDTLS_PREFIX)/lib) -lm $(if $(filter android-%,$(platform)),,-lpthread)

OBJ_DIR := $(BUILD_DIR)/obj
OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES_C) $(EXTRA_SOURCES))

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
HARNESS     := $(BUILD_DIR)/harness$(EXE)
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
	@RUN="$(RUN)" sh tests/check.sh ./$(TARGET) $(HARNESS) $(FIXTURE_DIR) $(BUILD_DIR)

.PHONY: harness fixtures check
