# Makefile for PV Performance Database Tools

CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -I. -I../src

# SQLite3 (try multiple methods to find headers)
# Calculate path to project root: from offline_trainning -> simulation_preparations -> src -> TRACE-PV
SQLITE3_PREFIX := $(shell dirname $(shell dirname $(shell dirname $(shell pwd))))/sqlite3
SQLITE3_CFLAGS := $(shell pkg-config --cflags sqlite3 2>/dev/null)
ifeq ($(SQLITE3_CFLAGS),)
  # Try project local installation first, then system paths
  SQLITE3_CFLAGS := $(shell if [ -f $(SQLITE3_PREFIX)/include/sqlite3.h ]; then echo "-I$(SQLITE3_PREFIX)/include"; elif [ -f /usr/include/sqlite3.h ]; then echo ""; elif [ -f /usr/local/include/sqlite3.h ]; then echo "-I/usr/local/include"; else echo ""; fi)
endif
SQLITE3_LIBS := $(shell pkg-config --libs sqlite3 2>/dev/null || if [ -f $(SQLITE3_PREFIX)/lib/libsqlite3.so ]; then echo "-L$(SQLITE3_PREFIX)/lib -lsqlite3"; else echo "-lsqlite3"; fi)

# All includes (no external JSON library needed)
INCLUDES := $(SQLITE3_CFLAGS) -I../src

# Source files
OFFLINE_GEN_SRCS := offline_data_generator.cpp pv_performance_db.cpp single_diode_model.cpp
SIMULATOR_SRCS := iv_curve_simulator.cpp pv_performance_db.cpp
CASE_STUDY_SRCS := case_study_example.cpp iv_curve_simulator.cpp pv_performance_db.cpp
USAGE_EXAMPLE_SRCS := usage_example.cpp iv_curve_simulator.cpp pv_performance_db.cpp

# Object files
OFFLINE_GEN_OBJS := $(OFFLINE_GEN_SRCS:.cpp=.o)
SIMULATOR_OBJS := $(SIMULATOR_SRCS:.cpp=.o)
CASE_STUDY_OBJS := $(CASE_STUDY_SRCS:.cpp=.o)
USAGE_EXAMPLE_OBJS := $(USAGE_EXAMPLE_SRCS:.cpp=.o)

# Targets
TARGETS := offline_data_generator case_study_example usage_example

.PHONY: all clean check-sqlite3

# Check for SQLite3 headers (warning only)
check-sqlite3:
	@SQLITE3_PREFIX=$$(dirname $$(dirname $$(dirname $$(pwd))))/sqlite3; \
	if ! pkg-config --exists sqlite3 2>/dev/null && \
	   [ ! -f $$SQLITE3_PREFIX/include/sqlite3.h ] && \
	   [ ! -f /usr/include/sqlite3.h ] && \
	   [ ! -f /usr/local/include/sqlite3.h ]; then \
		echo "WARNING: sqlite3.h not found in standard locations."; \
		echo "  If compilation fails, please install libsqlite3-dev:"; \
		echo "    sudo apt-get install libsqlite3-dev  # Debian/Ubuntu"; \
		echo "    sudo yum install sqlite-devel        # CentOS/RHEL"; \
	fi

.PHONY: all clean

all: check-sqlite3 $(TARGETS)

offline_data_generator: $(OFFLINE_GEN_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE3_LIBS)

iv_curve_simulator_test: $(SIMULATOR_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE3_LIBS)

case_study_example: $(CASE_STUDY_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE3_LIBS)

usage_example: $(USAGE_EXAMPLE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(SQLITE3_LIBS)

# Compile rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(TARGETS) *.o

# Example usage
.PHONY: example
example: all
	@echo "Example usage:"
	@echo "  1. Generate database (use component_database location):"
	@echo "     ./offline_data_generator ../../../component_database/component_parameters.db ../../../component_database/pv_system"
	@echo "  2. Run case study:"
	@echo "     ./case_study_example ../../../component_database/component_parameters.db \"CHSM66M-HC (210)\""

