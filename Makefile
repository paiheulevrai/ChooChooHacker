CXX = aarch64-linux-gnu-g++
CPPFLAGS = $(shell PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig pkg-config --cflags sdl2)
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Werror -pthread
LDLIBS = -lSDL2 -pthread
TARGET = build/choochoohacker.aarch64

.PHONY: all test package
all: $(TARGET)

$(TARGET): src/main.cpp src/font12x16.cpp src/audio_test.cpp src/audio_test.h | build/control.sh
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/main.cpp src/font12x16.cpp src/audio_test.cpp $(LDLIBS) -o $@

build/audio-test: tests/audio_test.cpp src/audio_test.cpp src/audio_test.h
	mkdir -p build
	g++ $(shell pkg-config --cflags sdl2) $(CXXFLAGS) tests/audio_test.cpp src/audio_test.cpp $(shell pkg-config --libs sdl2) -o $@

build/control.sh: control.sh
	mkdir -p build
	cp control.sh $@

build/ui-test: src/main.cpp src/font12x16.cpp src/audio_test.cpp src/audio_test.h | build/control.sh
	mkdir -p build
	g++ $(shell pkg-config --cflags sdl2) $(CXXFLAGS) src/main.cpp src/font12x16.cpp src/audio_test.cpp $(shell pkg-config --libs sdl2) -o $@

build/audio-hardware-test: tests/audio_test.cpp src/audio_test.cpp src/audio_test.h
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/audio_test.cpp src/audio_test.cpp $(LDLIBS) -o $@

test: build/audio-test build/ui-test
	python3 -m unittest discover -s tests -v
	SDL_AUDIODRIVER=dummy ./build/audio-test
	./build/ui-test --self-test

package: $(TARGET)
	mkdir -p build/ChooChooHacker.pak/licenses releases build/portmaster/choochoohacker
	cp $(TARGET) control.sh build/ChooChooHacker.pak/
	cp packaging/launch.sh build/ChooChooHacker.pak/
	cp LICENSE build/ChooChooHacker.pak/licenses/
	cp LICENSE build/ChooChooHacker.pak/
	cp README.md build/ChooChooHacker.pak/
	cp -r docs build/ChooChooHacker.pak/
	chmod +x build/ChooChooHacker.pak/launch.sh build/ChooChooHacker.pak/control.sh
	cd build && zip -qr -FS ../releases/ChooChooHacker.pak.zip ChooChooHacker.pak
	cp -r build/ChooChooHacker.pak/. build/portmaster/choochoohacker/
	rm -f build/portmaster/choochoohacker/launch.sh
	cp packaging/ChooChooHacker.sh build/portmaster/
	chmod +x build/portmaster/ChooChooHacker.sh
	cd build/portmaster && zip -qr -FS ../../releases/choochoohacker-portmaster.zip .
