CXX := clang++
CC := clang

BIN_DIR := bin
BUILD_DIR := build
TARGET := $(BIN_DIR)/redix
DEBUG_TARGET := $(BIN_DIR)/redix-debug
TEST_TARGET := $(BUILD_DIR)/test/redix_tests

CPP_SOURCES := $(wildcard src/*.cpp)
C_SOURCES := $(wildcard src/*.c)
TEST_SOURCES := $(wildcard test/*_tests.cpp)
LIB_CPP_SOURCES := $(filter-out src/main.cpp,$(CPP_SOURCES))

COMMON_CXXFLAGS := -std=c++23 -stdlib=libc++ -Wall -Wextra -Wpedantic -Iinclude
COMMON_CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -Iinclude
RELEASE_FLAGS := -O2
DEBUG_FLAGS := -g -O0 -fsanitize=address,undefined
DEBUG_CFLAGS := -g -O0 -fsanitize=address,undefined
COMMON_LDFLAGS := -stdlib=libc++
DEBUG_LDFLAGS := -fsanitize=address,undefined
TEST_LDLIBS := -lcriterion

RELEASE_CPP_OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/release/%.o,$(CPP_SOURCES))
RELEASE_C_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/release/%.o,$(C_SOURCES))
DEBUG_CPP_OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/debug/%.o,$(CPP_SOURCES))
DEBUG_C_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/debug/%.o,$(C_SOURCES))
TEST_CPP_OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/test/%.o,$(LIB_CPP_SOURCES)) \
                    $(patsubst test/%.cpp,$(BUILD_DIR)/test/%.o,$(TEST_SOURCES))
TEST_C_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/test/%.o,$(C_SOURCES))

.PHONY: all release debug test clean integration

all: release

release: $(TARGET)

debug: $(DEBUG_TARGET)

$(TARGET): $(RELEASE_CPP_OBJECTS) $(RELEASE_C_OBJECTS) | $(BIN_DIR)
	$(CXX) $(COMMON_LDFLAGS) $^ -o $@

$(DEBUG_TARGET): $(DEBUG_CPP_OBJECTS) $(DEBUG_C_OBJECTS) | $(BIN_DIR)
	$(CXX) $(COMMON_LDFLAGS) $(DEBUG_LDFLAGS) $^ -o $@

$(TEST_TARGET): $(TEST_CPP_OBJECTS) $(TEST_C_OBJECTS) | $(BUILD_DIR)/test
	$(CXX) $(COMMON_LDFLAGS) $(DEBUG_LDFLAGS) $^ $(TEST_LDLIBS) -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

integration: release
	./test/integration.sh

$(BUILD_DIR)/release/%.o: src/%.cpp | $(BUILD_DIR)/release
	$(CXX) $(COMMON_CXXFLAGS) $(RELEASE_FLAGS) -c $< -o $@

$(BUILD_DIR)/release/%.o: src/%.c | $(BUILD_DIR)/release
	$(CC) $(COMMON_CFLAGS) $(RELEASE_FLAGS) -c $< -o $@

$(BUILD_DIR)/debug/%.o: src/%.cpp | $(BUILD_DIR)/debug
	$(CXX) $(COMMON_CXXFLAGS) $(DEBUG_FLAGS) -c $< -o $@

$(BUILD_DIR)/debug/%.o: src/%.c | $(BUILD_DIR)/debug
	$(CC) $(COMMON_CFLAGS) $(DEBUG_CFLAGS) -c $< -o $@

$(BUILD_DIR)/test/%.o: src/%.cpp | $(BUILD_DIR)/test
	$(CXX) $(COMMON_CXXFLAGS) $(DEBUG_FLAGS) -c $< -o $@

$(BUILD_DIR)/test/%.o: test/%.cpp | $(BUILD_DIR)/test
	$(CXX) $(COMMON_CXXFLAGS) $(DEBUG_FLAGS) -c $< -o $@

$(BUILD_DIR)/test/%.o: src/%.c | $(BUILD_DIR)/test
	$(CC) $(COMMON_CFLAGS) $(DEBUG_CFLAGS) -c $< -o $@

$(BIN_DIR) $(BUILD_DIR)/release $(BUILD_DIR)/debug $(BUILD_DIR)/test:
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)/redix $(BIN_DIR)/redix-debug
