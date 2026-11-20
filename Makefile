CXX ?= c++
CXXFLAGS ?= -O0 -g -Wall

repro: repro.cpp
	$(CXX) $(CXXFLAGS) -o repro repro.cpp

# -O0 keeps the loop from being optimised away. The guard page makes the fault
# deterministic, so a successful run is a non-zero exit (139 = SIGSEGV on Linux).
run: repro
	-./repro; echo "exit: $$?"

clean:
	rm -f repro

.PHONY: run clean
