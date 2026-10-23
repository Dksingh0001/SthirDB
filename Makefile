# SthirDB build. `make` produces ./build/sthirdb; `make test` builds and runs unit tests.

ifeq ($(OS),Windows_NT)
    EXE := .exe

    # Locate POSIX shell and utilities on Windows (Git for Windows, MSYS2) if available
    ifneq ($(wildcard C:/Program Files/Git/usr/bin/sh.exe),)
        POSIX_USR_BIN := C:/Program Files/Git/usr/bin
    else ifneq ($(wildcard C:/msys64/usr/bin/sh.exe),)
        POSIX_USR_BIN := C:/msys64/usr/bin
    else ifneq ($(wildcard $(LOCALAPPDATA)/Programs/Git/usr/bin/sh.exe),)
        POSIX_USR_BIN := $(LOCALAPPDATA)/Programs/Git/usr/bin
    else ifneq ($(wildcard C:/Program Files (x86)/Git/usr/bin/sh.exe),)
        POSIX_USR_BIN := C:/Program Files (x86)/Git/usr/bin
    else ifneq ($(wildcard C:/msys/usr/bin/sh.exe),)
        POSIX_USR_BIN := C:/msys/usr/bin
    endif

    ifneq ($(POSIX_USR_BIN),)
        SHELL := $(POSIX_USR_BIN)/sh.exe
        export PATH := $(POSIX_USR_BIN);$(PATH)
    endif

    MKDIR_P := mkdir -p
    RM_RF   := rm -rf
else
    EXE     :=
    MKDIR_P := mkdir -p
    RM_RF   := rm -rf
endif

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -Isrc -MMD -MP
BUILD    := build

SRCS := $(wildcard src/*.cpp src/*/*.cpp)
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Target executable
TARGET := $(BUILD)/sthirdb$(EXE)

# Tests link everything except the CLI entry point.
STORAGE_OBJS := $(filter-out %/main.o,$(OBJS))
TEST_SRCS    := $(wildcard test/*.cpp)
TEST_BINS    := $(patsubst test/%.cpp,$(BUILD)/%$(EXE),$(TEST_SRCS))

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@$(MKDIR_P) $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/%.o: %.cpp
	@$(MKDIR_P) $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test_%$(EXE): test/test_%.cpp $(STORAGE_OBJS)
	@$(MKDIR_P) $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $< $(STORAGE_OBJS)

# Convenience aliases when omitting .exe on Windows
ifneq ($(EXE),)
$(BUILD)/sthirdb: $(TARGET)
$(BUILD)/test_%: $(BUILD)/test_%$(EXE)
endif

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

clean:
	$(RM_RF) $(BUILD)

-include $(DEPS)
