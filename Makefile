# Uses the Command Line Tools compiler, so neither Xcode nor CMake is needed.
CXX := $(shell DEVELOPER_DIR=/Library/Developer/CommandLineTools xcrun -f clang++)
SDK := $(shell DEVELOPER_DIR=/Library/Developer/CommandLineTools xcrun --show-sdk-path)
RT := third_party/dotnet-runtime/src
OUT := artifacts

# The defines the runtime's own build sets for the headers on osx-arm64.
DEFS := -DHOST_UNIX -DHOST_64BIT -DHOST_ARM64 -DHOST_APPLE -DHOST_OSX \
        -DTARGET_UNIX -DTARGET_64BIT -DTARGET_ARM64 -DTARGET_APPLE -DTARGET_OSX -DBIT64 \
        -DFEATURE_STANDALONE_GC -DBUILD_AS_STANDALONE -DFEATURE_MANUALLY_MANAGED_CARD_BUNDLES \
        -DFEATURE_USE_SOFTWARE_WRITE_WATCH_FOR_GC_HEAP -DFEATURE_EVENT_TRACE -D_DARWIN_C_SOURCE

CXXFLAGS := -std=c++17 -arch arm64 -isysroot $(SDK) -O2 -g -fPIC -fvisibility=hidden \
            -fno-exceptions -fno-rtti -fno-strict-aliasing -Wall -Wextra -Werror \
            $(DEFS) -Igc -isystem $(RT)/coreclr/gc -isystem $(RT)/coreclr/gc/env \
            -isystem $(RT)/coreclr/inc -isystem $(RT)/native

SRCS := $(wildcard gc/*.cpp)
OBJS := $(SRCS:gc/%.cpp=$(OUT)/obj/%.o)
LIB := $(OUT)/libmanualgc.dylib

.PHONY: all clean
all: $(LIB)

$(LIB): $(OBJS)
	$(CXX) -arch arm64 -isysroot $(SDK) -dynamiclib -install_name @rpath/libmanualgc.dylib -o $@ $^

$(OUT)/obj/%.o: gc/%.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
