# Else exist specifically for clang
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wno-narrowing
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland libinput libudev wayland-server xkbcommon librsvg-2.0`
LIBS = `pkg-config --libs librsvg-2.0`

SRC = main.cpp barDeco.cpp BarPassElement.cpp CSDManager.cpp AppIcon.cpp TopEdgeSnap.cpp MaximizeManager.cpp
TARGET = hyprtouchbar.so

all: $(TARGET)

$(TARGET): $(SRC) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(INCLUDES) $(SRC) -o $@ $(LIBS)

clean:
	rm -f ./$(TARGET)

meson-build:
	mkdir -p build
	cd build && meson .. && ninja

test: test-captions
	@bin=$$(mktemp /tmp/htb-policy-test.XXXXXX); trap 'rm -f "$$bin"' EXIT; \
	$(CXX) -std=c++23 -Wall -Wextra -Werror -I. tests/top_edge_policy.cpp -o "$$bin" && "$$bin"

test-captions:
	python3 tools/generate_caption_icons.py --check
	@bin=$$(mktemp /tmp/htb-caption-test.XXXXXX); trap 'rm -f "$$bin"' EXIT; \
	$(CXX) -std=c++23 -Wall -Wextra -Werror -I. tests/caption_icons.cpp $$(pkg-config --cflags --libs librsvg-2.0) -o "$$bin" && "$$bin"

.PHONY: all meson-build clean test test-captions
