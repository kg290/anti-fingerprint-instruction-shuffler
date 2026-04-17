CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

SRC := $(wildcard src/*.cpp)
OUT := build/afis

all: $(OUT)

build:
	mkdir -p build

$(OUT): $(SRC) | build
	$(CXX) $(CXXFLAGS) $(SRC) -o $(OUT)

clean:
	rm -rf build

.PHONY: all clean