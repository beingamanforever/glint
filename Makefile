CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra
PCRE2 := $(shell pcre2-config --cflags --libs8)

glint: src/glint.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(PCRE2) -pthread
