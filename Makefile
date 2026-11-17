# Basic idea is for each .cpp file to be compiled and assembled into a .o file
# under build/, matching the source tree 
#		src/storage/segment.cpp -> build/obj/src/storage.segment.o

# Common commands I use (or think I might)
# 	make -j4				Build everything 4 files at a time
# 	make test				Build and run all test programs. Stops at first failure
# 	make build/bin/broker	Build just one program
# 	make DEBUG=1			Debug mode
#	make SAN=address		AddressSanitizer
#	make SAN=thread			ThreadSanitizer
# 	make clean				Cleans (duh)

# ---------- Compiler Stuff ----------
CXX	?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pthread -Isrc -MMD -MP
LDFLAGS :=
LDLIBS := -pthread

# g++ 8 doesn't link std::filesystem, but clang on MacOS does
ifeq ($(shell uname -s), Linux)
    LDLIBS += -lstdc++fs
endif

ifeq ($(DEBUG), 1)
    CXXFLAGS += -O0 -g
else
    CXXFLAGS += -O2 -g
endif


# ---------- Layout ----------
BUILD := build
OBJ := $(BUILD)/obj
BIN := $(BUILD)/bin
TESTBIN := $(BUILD)/testbin
LIB := $(BUILD)/libtributary.a

LIB_SRCS := $(shell find src -name '*.cpp')
APP_SRCS := $(wildcard apps/*.cpp)
TEST_SRCS := $(wildcard tests/test_*.cpp)

LIB_OBJS := $(LIB_SRCS:%.cpp=$(OBJ)%.o)
APPS := $(APP_SRCS:apps/%.cpp=$(BIN)/%)
TESTS := $(TEST_SRCS:tests/%.cpp=$(TESTBIN)/%)
ALL_OBJS := $(LIB_OBJS) $(APP_SRCS:%.cpp=$(OBJ)/%.o) $(TEST_SRCS:%.cpp=$(OBJ)/%.o)


# ---------- Targets ----------
.PHONY: all apps tests test clean
all: apps tests
apps: $(APPS)
tests: $(TESTS)

test: tests
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done
	@echo "All tests passed."

clean:
	rm -rf $(BUILD)


# ---------- Rules ----------
$(LIB): $(LIB_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(TESTBIN)/%: $(OBJ)/tests/%.o $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

.SECONDARY:
-include $(ALL_OBJS:.o=.d)
