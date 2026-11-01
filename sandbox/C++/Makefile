MAKEFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
MAKEFILE_DIR  := $(dir $(MAKEFILE_PATH))
INCLUDE_DIR := $(MAKEFILE_DIR)/../../umbridge/lib/  
CXX=g++
CXXFLAGS=-std=c++17 -Wall -pthread
CPPFLAGS=-std=c++17 -I$(INCLUDE_DIR)
LDLIBS=-lpthread -lssl -lcrypto
all: minimal-server http-client
minimal-server: minimal-server.cpp
http-client: http-client.cpp
