# SthirDB build. `make` produces ./build/sthirdb; `make test` builds and runs unit tests.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -Isrc -MMD -MP
BUILD    := build

SRCS := $(wildcard src/*.cpp src/*/*.cpp)
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Tests link everything except the CLI entry point.
STORAGE_OBJS := $(filter-out $(BUILD)/main.o,$(OBJS))
TEST_SRCS    := $(wildcard test/*.cpp)
TEST_BINS    := $(patsubst test/%.cpp,$(BUILD)/%,$(TEST_SRCS))

.PHONY: all test clean

all: $(BUILD)/sthirdb

$(BUILD)/sthirdb: $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test_%: test/test_%.cpp $(STORAGE_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $< $(STORAGE_OBJS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

clean:
	rm -rf $(BUILD)

-include $(DEPS)
