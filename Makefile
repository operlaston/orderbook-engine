CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wno-interference-size -O2 -g -MMD -MP -pthread
GTEST_LIBS := -lgtest -lgtest_main -pthread
LDFLAGS := -pthread
INCLUDES := -I include

# make          -> release build in build/release (debug prints compiled out)
# make DEBUG=1  -> debug build in build/debug   (debug prints active)
DEBUG ?= 0
ifeq ($(DEBUG),1)
  BUILD := build/debug
  CXXFLAGS += -DDEBUG_BUILD -Og
else
  BUILD := build/release
endif

SERVER_SRCS := src/Orderbook.cpp src/Server.cpp src/main.cpp
SERVER_OBJS := $(SERVER_SRCS:src/%.cpp=$(BUILD)/%.o)

# Production objects reused by the tests (everything except main.o)
LIB_OBJS := $(BUILD)/Orderbook.o

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(TEST_SRCS:tests/%.cpp=$(BUILD)/tests/%.o)

# Each bench/*.cpp is its own standalone binary (own main()).
BENCH_SRCS := $(wildcard bench/*.cpp)
BENCH_BINS := $(BENCH_SRCS:bench/%.cpp=$(BUILD)/%)

.PHONY: all clean test bench

all: $(BUILD)/orderbook $(BUILD)/client

# orderbook

$(BUILD)/orderbook: $(SERVER_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# client
$(BUILD)/client: src/client/Client.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)

# tests
test: $(BUILD)/run_tests
	./$(BUILD)/run_tests

$(BUILD)/run_tests: $(TEST_OBJS) $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(GTEST_LIBS)

$(BUILD)/tests/%.o: tests/%.cpp | $(BUILD)/tests
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# bench
bench: $(BENCH_BINS)
	./$(BUILD)/benchmark
	./$(BUILD)/bench_app

# engine microbenchmark: links the matching engine directly, no networking
$(BUILD)/benchmark: bench/benchmark.cpp $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(LIB_OBJS) -o $@ $(LDFLAGS)

# whole-app benchmark: drives the real server binary over TCP
$(BUILD)/bench_app: bench/bench_app.cpp | $(BUILD)/orderbook $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DSERVER_BIN='"$(BUILD)/orderbook"' $< -o $@ $(LDFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: | $(BUILD)
	mkdir -p $(BUILD)/tests

clean:
	rm -rf build

-include $(SERVER_OBJS:.o=.d) $(BUILD)/Client.d $(TEST_OBJS:.o=.d) $(BENCH_BINS:=.d)
