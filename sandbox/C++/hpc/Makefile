MAKEFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
MAKEFILE_DIR  := $(dir $(MAKEFILE_PATH))
INCLUDE_DIR := $(MAKEFILE_DIR)/../../../umbridge/lib/  
CXX=g++
CXXFLAGS=-std=c++20 -O2 -pthread
CPPFLAGS=-std=c++20 -I$(INCLUDE_DIR) -I. 
LDLIBS=-lpthread -lssl -lcrypto
UMBRIDGE_HEADERS := $(wildcard $(strip $(INCLUDE_DIR))/*.h $(strip $(INCLUDE_DIR))/*.hpp)
all: LoadBalancer is_port_free testmodel http-client
LoadBalancer testmodel http-client: $(UMBRIDGE_HEADERS)
LoadBalancer: LoadBalancer.cpp LoadBalancer.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) LoadBalancer.cpp -o LoadBalancer $(LDLIBS)
is_port_free: is_port_free.cpp 
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) is_port_free.cpp -o is_port_free $(LDLIBS)
testmodel: testmodel.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) testmodel.cpp -o testmodel $(LDLIBS)
http-client: http-client.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) http-client.cpp -o http-client $(LDLIBS)
.PHONY: test-parallel
test-parallel: all
	python3 tests/parallel_sessions.py
# Needs pandoc, xelatex and mmdc; see make-guide-pdf.sh. Not part of 'all'.
PBS_Deployment_Guide.pdf: PBS_Deployment_Guide.md make-guide-pdf.sh
	./make-guide-pdf.sh $<
clean:
	rm -f LoadBalancer is_port_free testmodel http-client
	

