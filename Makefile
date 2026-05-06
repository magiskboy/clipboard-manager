BUILD_DIR := build
TARGET := $(BUILD_DIR)/clipboard-manager

LIBUI_VENDOR := vendors/libui-ng
LIBUI_BUILD := $(LIBUI_VENDOR)/build
LIBUI_OUT := $(LIBUI_BUILD)/meson-out

MESON ?= meson
NINJA ?= ninja
MESON_SETUP_ARGS := --buildtype=release -Dtests=false -Dexamples=false

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -pipe
LDFLAGS ?=

OS_UNAME := $(shell uname -s)
OS_MAKE := $(OS)

SRC_COMMON := main.c core.c

ifeq ($(OS_UNAME),Linux)
  SRC_PLATFORM := linux.c
  PLATFORM_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 x11 xtst)
  PLATFORM_LDFLAGS := $(shell pkg-config --libs gtk+-3.0 x11 xtst)
else ifeq ($(OS_UNAME),Darwin)
  SRC_PLATFORM := darwin.m
  PLATFORM_CFLAGS :=
  PLATFORM_LDFLAGS := -framework Cocoa
else ifeq ($(OS_MAKE),Windows_NT)
  SRC_PLATFORM := windows.c
  PLATFORM_CFLAGS :=
  PLATFORM_LDFLAGS := -lshell32 -luser32 -lkernel32 -lgdi32 -lcomctl32 -luxtheme -lmsimg32 -lcomdlg32 -ld2d1 -ldwrite -lole32 -loleaut32 -loleacc -luuid -lwindowscodecs
  MESON_SETUP_ARGS += -Ddefault_library=static
else ifneq (,$(findstring MINGW,$(OS_UNAME)))
  SRC_PLATFORM := windows.c
  PLATFORM_CFLAGS :=
  PLATFORM_LDFLAGS := -lshell32 -luser32 -lkernel32 -lgdi32 -lcomctl32 -luxtheme -lmsimg32 -lcomdlg32 -ld2d1 -ldwrite -lole32 -loleaut32 -loleacc -luuid -lwindowscodecs
  MESON_SETUP_ARGS += -Ddefault_library=static
else ifneq (,$(findstring MSYS,$(OS_UNAME)))
  SRC_PLATFORM := windows.c
  PLATFORM_CFLAGS :=
  PLATFORM_LDFLAGS := -lshell32 -luser32 -lkernel32 -lgdi32 -lcomctl32 -luxtheme -lmsimg32 -lcomdlg32 -ld2d1 -ldwrite -lole32 -loleaut32 -loleacc -luuid -lwindowscodecs
  MESON_SETUP_ARGS += -Ddefault_library=static
else
  $(error Unsupported platform: $(OS_UNAME) (set SRC_PLATFORM manually))
endif

SRC := $(SRC_COMMON) $(SRC_PLATFORM)

OBJ := $(BUILD_DIR)/main.o $(BUILD_DIR)/core.o
ifeq ($(SRC_PLATFORM),linux.c)
  OBJ += $(BUILD_DIR)/linux.o
endif
ifeq ($(SRC_PLATFORM),windows.c)
  OBJ += $(BUILD_DIR)/windows.o
endif
ifeq ($(SRC_PLATFORM),darwin.m)
  OBJ += $(BUILD_DIR)/darwin.o
endif

LIBUI_SHLIB :=
ifeq ($(OS_UNAME),Darwin)
  LIBUI_SHLIB := $(LIBUI_OUT)/libui.dylib
  LIBUI_RPATH := -Wl,-rpath,@loader_path/../vendors/libui-ng/build/meson-out
else ifneq (,$(filter Windows_NT MINGW% MSYS%,$(OS_MAKE) $(OS_UNAME)))
  LIBUI_SHLIB := $(LIBUI_OUT)/libui.a
  LIBUI_RPATH :=
else
  LIBUI_SHLIB := $(LIBUI_OUT)/libui.so
  LIBUI_RPATH := -Wl,-rpath,'$$ORIGIN/../vendors/libui-ng/build/meson-out'
endif

USE_SYSTEM_LIBUI ?=
LIBUI_PREFIX ?=

ifneq ($(strip $(USE_SYSTEM_LIBUI)),)
  LIBUI_CFLAGS := -I$(LIBUI_PREFIX)/include
  LIBUI_LDFLAGS := -L$(LIBUI_PREFIX)/lib -lui $(PLATFORM_LDFLAGS)
  LIBUI_DEPS :=
  LIBUI_HEADER := $(LIBUI_PREFIX)/include/ui.h
else
  LIBUI_CFLAGS := -I$(LIBUI_VENDOR) $(PLATFORM_CFLAGS)
  LIBUI_LDFLAGS := -L$(LIBUI_OUT) -lui $(LIBUI_RPATH) $(PLATFORM_LDFLAGS)
  LIBUI_DEPS := $(LIBUI_SHLIB)
  LIBUI_HEADER := $(LIBUI_VENDOR)/ui.h
endif

ALL_CFLAGS := $(CFLAGS) -I. $(LIBUI_CFLAGS)

.PHONY: all clean distclean libui

all: $(TARGET)

ifneq ($(strip $(USE_SYSTEM_LIBUI)),)
libui:
	@echo 'USE_SYSTEM_LIBUI is set; skipping vendor libui-ng build.' >&2
else
$(LIBUI_BUILD)/build.ninja: $(LIBUI_VENDOR)/meson.build
	cd $(LIBUI_VENDOR) && \
	if [ -f build/build.ninja ]; then \
		$(MESON) setup build --reconfigure $(MESON_SETUP_ARGS); \
	else \
		$(MESON) setup build $(MESON_SETUP_ARGS); \
	fi

$(LIBUI_SHLIB): $(LIBUI_BUILD)/build.ninja
	cd $(LIBUI_VENDOR) && $(NINJA) -C build

libui: $(LIBUI_SHLIB)
endif

$(TARGET): $(OBJ) $(LIBUI_DEPS)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBUI_LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/main.o: main.c $(LIBUI_HEADER) clipboard_manager.h platform.h | $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ main.c

$(BUILD_DIR)/core.o: core.c clipboard_manager.h | $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ core.c

$(BUILD_DIR)/linux.o: linux.c platform.h | $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ linux.c

$(BUILD_DIR)/windows.o: windows.c platform.h | $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ windows.c

$(BUILD_DIR)/darwin.o: darwin.m platform.h | $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -fobjc-arc -c -o $@ darwin.m

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(LIBUI_BUILD)
