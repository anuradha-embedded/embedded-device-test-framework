# Cmake-free build for the Embedded Device Test Framework.
#
# Targets:
#   make            -> build the example_suite binary and the self-tests
#   make test       -> build everything and run the self-tests + example checks
#   make example    -> just the demonstration suite
#   make clean      -> remove build artifacts

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -Iexamples
LDFLAGS  ?= -pthread

BUILD    := build-make
LIB_SRCS := src/registry.cpp src/reporter.cpp src/runner.cpp
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(LIB_SRCS))

EXAMPLE_BIN := $(BUILD)/example_suite
TEST_BIN    := $(BUILD)/framework_tests

.PHONY: all example test clean

all: $(EXAMPLE_BIN) $(TEST_BIN)

example: $(EXAMPLE_BIN)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(EXAMPLE_BIN): examples/example_suite.cpp $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(LIB_OBJS) -o $@ $(LDFLAGS)

$(TEST_BIN): tests/framework_tests.cpp $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< $(LIB_OBJS) -o $@ $(LDFLAGS)

# Run the self-tests, then confirm the example suite passes with the slow demo
# excluded. The full example suite intentionally contains a timeout case, so it
# is exercised separately via --list here.
test: all
	@echo "== framework self-tests =="
	./$(TEST_BIN)
	@echo "== example suite (listing) =="
	./$(EXAMPLE_BIN) --list
	@echo "== example suite (excluding slow) =="
	./$(EXAMPLE_BIN) --exclude-tag slow

clean:
	$(RM) -r $(BUILD)
