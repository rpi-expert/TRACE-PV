CUDA_PATH ?= /usr/local/cuda
NVCC      := $(CUDA_PATH)/bin/nvcc
GPU_SM    ?= $(shell nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
NVCC_ARCH := $(if $(GPU_SM),-arch=sm_$(GPU_SM),)

TARGET    := trace_pv
BIN_DIR   := bin
SRC_DIR   := src

CPP_SRCS  := $(SRC_DIR)/main.cpp \
             $(SRC_DIR)/simulation_params.cpp \
             $(SRC_DIR)/simulation_case.cpp \
	             $(SRC_DIR)/simulation_model.cpp \
             $(SRC_DIR)/model_validation/intermediate_value_exporter.cpp \
             $(SRC_DIR)/reporting/run_report.cpp \
	             $(SRC_DIR)/multi_physics_simulator/electrical_simulation/modulation.cpp \
             $(SRC_DIR)/multi_physics_simulator/electrical_simulation/gpu_capacity.cpp \
             $(SRC_DIR)/multi_physics_simulator/environmental_simulation/internal_conditions.cpp \
             $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_loss_thermal_model.cpp \
             $(SRC_DIR)/multi_physics_simulator/thermal_simulation/igbt_loss_thermal_model.cpp \
             $(SRC_DIR)/simulation_preparation/pv_voltage_iv_curve.cpp \
             $(SRC_DIR)/simulation_preparation/mission_profile_loader.cpp \
             $(SRC_DIR)/reliability_assessment/pcb_reliability.cpp \
             $(SRC_DIR)/reliability_assessment/rainflow_counting.cpp \
             $(SRC_DIR)/reliability_assessment/reliability_models.cpp \
             component_database/component_database.cpp

CU_SRCS   := $(SRC_DIR)/multi_physics_simulator/electrical_simulation/a2s_gpu.cu \
	             $(SRC_DIR)/multi_physics_simulator/electrical_simulation/stress_calculation.cu \
	             $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_reference_gpu.cu \
	             $(SRC_DIR)/multi_physics_simulator/thermal_simulation/igbt_reference_gpu.cu \
             $(SRC_DIR)/reliability_assessment/capacitor_reliability.cu \
             $(SRC_DIR)/reliability_assessment/fan_cooling_reliability.cu \
             $(SRC_DIR)/reliability_assessment/igbt_reliability.cu \
             $(SRC_DIR)/reliability_assessment/reliability_kernels.cu

# Create object file names in bin directory
OBJS      := $(BIN_DIR)/main.o \
             $(BIN_DIR)/simulation_params.o \
             $(BIN_DIR)/simulation_case.o \
	             $(BIN_DIR)/simulation_model.o \
             $(BIN_DIR)/intermediate_value_exporter.o \
             $(BIN_DIR)/run_report.o \
	             $(BIN_DIR)/modulation.o \
             $(BIN_DIR)/gpu_capacity.o \
             $(BIN_DIR)/internal_conditions.o \
             $(BIN_DIR)/capacitor_loss_thermal_model.o \
             $(BIN_DIR)/igbt_loss_thermal_model.o \
             $(BIN_DIR)/pv_voltage_iv_curve.o \
	             $(BIN_DIR)/mission_profile_loader.o \
	             $(BIN_DIR)/stress_calculation.o \
	             $(BIN_DIR)/capacitor_reference_gpu.o \
	             $(BIN_DIR)/igbt_reference_gpu.o \
             $(BIN_DIR)/capacitor_reliability.o \
             $(BIN_DIR)/fan_cooling_reliability.o \
             $(BIN_DIR)/igbt_reliability.o \
             $(BIN_DIR)/reliability_kernels.o \
             $(BIN_DIR)/pcb_reliability.o \
             $(BIN_DIR)/rainflow_counting.o \
             $(BIN_DIR)/reliability_models.o \
             $(BIN_DIR)/a2s_gpu.o \
             $(BIN_DIR)/component_database.o

TARGET_PATH := $(BIN_DIR)/$(TARGET)

# SQLite3 configuration
SQLITE3_PREFIX := $(shell pwd)/sqlite3
SQLITE3_CFLAGS := $(shell if [ -f $(SQLITE3_PREFIX)/include/sqlite3.h ]; then echo "-I$(SQLITE3_PREFIX)/include"; elif pkg-config --exists sqlite3 2>/dev/null; then pkg-config --cflags sqlite3; else echo ""; fi)
SQLITE3_LIBS := $(shell if [ -f $(SQLITE3_PREFIX)/lib/libsqlite3.so ]; then echo "-L$(SQLITE3_PREFIX)/lib -Xlinker -rpath -Xlinker $(SQLITE3_PREFIX)/lib -lsqlite3"; elif pkg-config --exists sqlite3 2>/dev/null; then pkg-config --libs sqlite3; else echo "-lsqlite3"; fi)

CXX_FLAGS    := -std=c++17 -O2 -I. -I$(SRC_DIR) -I$(SRC_DIR)/multi_physics_simulator/electrical_simulation -I$(SRC_DIR)/multi_physics_simulator/environmental_simulation -I$(SRC_DIR)/simulation_preparation -I$(SRC_DIR)/reliability_assessment -Icomponent_database -Icomponent_database/offline_trainning $(SQLITE3_CFLAGS)
NVCC_FLAGS   := -std=c++17 -O2 -I. -I$(SRC_DIR) -I$(SRC_DIR)/multi_physics_simulator/electrical_simulation -I$(SRC_DIR)/multi_physics_simulator/environmental_simulation -I$(SRC_DIR)/simulation_preparation -I$(SRC_DIR)/reliability_assessment -Icomponent_database -Icomponent_database/offline_trainning $(SQLITE3_CFLAGS) $(NVCC_ARCH) -Xcompiler -fopenmp -allow-unsupported-compiler -Xcompiler -Wno-error -lcudart -lgomp
NVCC_DC_FLAGS := $(NVCC_FLAGS) -dc  # Device code compilation flag for separate compilation

all: $(TARGET_PATH)

# Device link objects (CUDA files that need device linking)
DEVICE_LINK_OBJS := $(BIN_DIR)/a2s_gpu_dlink.o $(BIN_DIR)/stress_calculation_dlink.o $(BIN_DIR)/capacitor_reliability_dlink.o $(BIN_DIR)/fan_cooling_reliability_dlink.o $(BIN_DIR)/igbt_reliability_dlink.o $(BIN_DIR)/reliability_kernels_dlink.o

$(TARGET_PATH): $(OBJS)
	@mkdir -p $(BIN_DIR)
	# First, create device link objects from CUDA object files
	$(NVCC) $(NVCC_FLAGS) -dlink $(BIN_DIR)/a2s_gpu.o $(BIN_DIR)/stress_calculation.o $(BIN_DIR)/capacitor_reference_gpu.o $(BIN_DIR)/igbt_reference_gpu.o $(BIN_DIR)/capacitor_reliability.o $(BIN_DIR)/fan_cooling_reliability.o $(BIN_DIR)/igbt_reliability.o $(BIN_DIR)/reliability_kernels.o -o $(BIN_DIR)/device_link.o
	# Then link everything together
	$(NVCC) $(NVCC_FLAGS) $(OBJS) $(BIN_DIR)/device_link.o -o $@ $(SQLITE3_LIBS)

# Compile main.cpp
$(BIN_DIR)/main.o: $(SRC_DIR)/main.cpp \
                   $(SRC_DIR)/simulation_preparation/iv_database.h \
                   $(SRC_DIR)/simulation_model.h \
                   $(SRC_DIR)/model_validation/intermediate_value_exporter.h \
                   $(SRC_DIR)/reporting/run_report.h \
                   $(SRC_DIR)/reporting/console_output.h \
                   $(SRC_DIR)/multi_physics_simulator/electrical_simulation/a2s_gpu.h \
                   $(SRC_DIR)/multi_physics_simulator/electrical_simulation/stress_calculation.h \
                   $(SRC_DIR)/multi_physics_simulator/environmental_simulation/internal_conditions.h \
                   $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_loss_thermal_model.h \
                   $(SRC_DIR)/multi_physics_simulator/thermal_simulation/simplified_loss_thermal.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile simulation_params.cpp
$(BIN_DIR)/simulation_params.o: $(SRC_DIR)/simulation_params.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile simulation_case.cpp
$(BIN_DIR)/simulation_case.o: $(SRC_DIR)/simulation_case.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile simulation_model.cpp
$(BIN_DIR)/simulation_model.o: $(SRC_DIR)/simulation_model.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile model-validation CSV exporter
$(BIN_DIR)/intermediate_value_exporter.o: $(SRC_DIR)/model_validation/intermediate_value_exporter.cpp $(SRC_DIR)/model_validation/intermediate_value_exporter.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

$(BIN_DIR)/run_report.o: $(SRC_DIR)/reporting/run_report.cpp $(SRC_DIR)/reporting/run_report.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile modulation.cpp
$(BIN_DIR)/modulation.o: $(SRC_DIR)/multi_physics_simulator/electrical_simulation/modulation.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile gpu_capacity.cpp
$(BIN_DIR)/gpu_capacity.o: $(SRC_DIR)/multi_physics_simulator/electrical_simulation/gpu_capacity.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile internal_conditions.cpp
$(BIN_DIR)/internal_conditions.o: $(SRC_DIR)/multi_physics_simulator/environmental_simulation/internal_conditions.cpp $(SRC_DIR)/multi_physics_simulator/environmental_simulation/internal_conditions.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile IGBT reference loss/thermal model
$(BIN_DIR)/capacitor_loss_thermal_model.o: $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_loss_thermal_model.cpp $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_loss_thermal_model.h $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_reference_gpu.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

$(BIN_DIR)/igbt_loss_thermal_model.o: $(SRC_DIR)/multi_physics_simulator/thermal_simulation/igbt_loss_thermal_model.cpp $(SRC_DIR)/reporting/console_output.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile pv_voltage_iv_curve.cpp
$(BIN_DIR)/pv_voltage_iv_curve.o: $(SRC_DIR)/simulation_preparation/pv_voltage_iv_curve.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile mission_profile_loader.cpp
$(BIN_DIR)/mission_profile_loader.o: $(SRC_DIR)/simulation_preparation/mission_profile_loader.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile a2s_gpu.cu (with device code flag for separate compilation)
$(BIN_DIR)/a2s_gpu.o: $(SRC_DIR)/multi_physics_simulator/electrical_simulation/a2s_gpu.cu $(SRC_DIR)/multi_physics_simulator/electrical_simulation/a2s_gpu.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

# Compile stress_calculation.cu (with device code flag for separate compilation)
$(BIN_DIR)/stress_calculation.o: $(SRC_DIR)/multi_physics_simulator/electrical_simulation/stress_calculation.cu $(SRC_DIR)/multi_physics_simulator/electrical_simulation/stress_calculation.h $(SRC_DIR)/multi_physics_simulator/electrical_simulation/a2s_gpu.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

$(BIN_DIR)/capacitor_reference_gpu.o: $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_reference_gpu.cu $(SRC_DIR)/multi_physics_simulator/thermal_simulation/capacitor_reference_gpu.h
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

$(BIN_DIR)/igbt_reference_gpu.o: $(SRC_DIR)/multi_physics_simulator/thermal_simulation/igbt_reference_gpu.cu
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

# Compile component_database.cpp
$(BIN_DIR)/component_database.o: component_database/component_database.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile capacitor_reliability.cu (with device code flag for separate compilation)
$(BIN_DIR)/capacitor_reliability.o: $(SRC_DIR)/reliability_assessment/capacitor_reliability.cu
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

# Compile fan_cooling_reliability.cu (with device code flag for separate compilation)
$(BIN_DIR)/fan_cooling_reliability.o: $(SRC_DIR)/reliability_assessment/fan_cooling_reliability.cu
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

# Compile igbt_reliability.cu (with device code flag for separate compilation)
$(BIN_DIR)/igbt_reliability.o: $(SRC_DIR)/reliability_assessment/igbt_reliability.cu
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

# Compile reliability_kernels.cu (with device code flag for separate compilation)
$(BIN_DIR)/reliability_kernels.o: $(SRC_DIR)/reliability_assessment/reliability_kernels.cu
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_DC_FLAGS) -c $< -o $@

# Compile pcb_reliability.cpp
$(BIN_DIR)/pcb_reliability.o: $(SRC_DIR)/reliability_assessment/pcb_reliability.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile rainflow_counting.cpp
$(BIN_DIR)/rainflow_counting.o: $(SRC_DIR)/reliability_assessment/rainflow_counting.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

# Compile reliability_models.cpp
$(BIN_DIR)/reliability_models.o: $(SRC_DIR)/reliability_assessment/reliability_models.cpp
	@mkdir -p $(BIN_DIR)
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@

run: $(TARGET_PATH)
	@if [ -z "$(TOPOLOGY)" ]; then \
		echo "Usage: make run TOPOLOGY=<2l2s|2l1s|3l2s|3l1s> [MODE=mission|static] [CSV=<file.csv>] [ROUNDS=<N>] [MOD=<svm|spwm>] [NGPUS=<N>]"; \
		exit 1; \
	fi
	@if [ -z "$(ROUNDS)" ]; then \
		ROUNDS=1; \
	fi
	@if [ -z "$(MOD)" ]; then \
		MOD=svm; \
	fi
	@if [ -z "$(MODE)" ]; then \
		MODE=mission; \
	fi
	@if [ -z "$(NGPUS)" ]; then \
		if [ "$(MODE)" = "static" ]; then \
			$(TARGET_PATH) --topology $(TOPOLOGY) --input-mode static --rounds $(ROUNDS) --modulation $(MOD); \
		elif [ -n "$(CSV)" ]; then \
			$(TARGET_PATH) --topology $(TOPOLOGY) --mission-csv $(CSV) --rounds $(ROUNDS) --modulation $(MOD); \
		else \
			$(TARGET_PATH) --topology $(TOPOLOGY) --input-mode mission --rounds $(ROUNDS) --modulation $(MOD); \
		fi; \
	else \
		if [ "$(MODE)" = "static" ]; then \
			$(TARGET_PATH) --topology $(TOPOLOGY) --input-mode static --rounds $(ROUNDS) --modulation $(MOD) --ngpus $(NGPUS); \
		elif [ -n "$(CSV)" ]; then \
			$(TARGET_PATH) --topology $(TOPOLOGY) --mission-csv $(CSV) --rounds $(ROUNDS) --modulation $(MOD) --ngpus $(NGPUS); \
		else \
			$(TARGET_PATH) --topology $(TOPOLOGY) --input-mode mission --rounds $(ROUNDS) --modulation $(MOD) --ngpus $(NGPUS); \
		fi; \
	fi

clean:
	rm -f $(OBJS) $(TARGET_PATH) $(BIN_DIR)/device_link.o
	find $(BIN_DIR) -name "*.o" -type f -delete 2>/dev/null || true

# Output/reporting contract tests run on a C++17 host without CUDA.
HOST_CXX ?= c++
HOST_TEST_DIR := $(BIN_DIR)/host-tests

test-reporting:
	@mkdir -p $(HOST_TEST_DIR)
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -I$(SRC_DIR) tests/run_report_test.cpp $(SRC_DIR)/reporting/run_report.cpp -o $(HOST_TEST_DIR)/run_report_test
	$(HOST_TEST_DIR)/run_report_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -pthread -I$(SRC_DIR) tests/console_output_test.cpp -o $(HOST_TEST_DIR)/console_output_test
	$(HOST_TEST_DIR)/console_output_test
	CXX="$(HOST_CXX)" python3 tests/output_cli_test.py

test-loss-thermal-cleanup:
	@mkdir -p $(HOST_TEST_DIR)
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -I$(SRC_DIR) tests/simplified_loss_thermal_test.cpp -o $(HOST_TEST_DIR)/simplified_loss_thermal_test
	$(HOST_TEST_DIR)/simplified_loss_thermal_test

.PHONY: all run clean test-reporting test-loss-thermal-cleanup

# CPU-only validation of mission cleaning and the runtime IV database.
.PHONY: test-mission-iv
test-mission-iv:
	@mkdir -p $(HOST_TEST_DIR)
	$(HOST_CXX) -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -I$(SRC_DIR) $(SQLITE3_CFLAGS) tests/mission_iv_input_test.cpp $(SRC_DIR)/simulation_preparation/mission_profile_loader.cpp $(SRC_DIR)/simulation_model.cpp -lsqlite3 -o $(HOST_TEST_DIR)/mission_iv_input_test
	$(HOST_TEST_DIR)/mission_iv_input_test
