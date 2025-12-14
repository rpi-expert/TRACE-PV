#!/bin/bash

# Script to run all configuration combinations for trace_pv simulator
# Usage: ./run_all_configs.sh [ROUNDS] [NGPUS] [MODEL_FILE]
#   ROUNDS: Number of rounds to process (default: 6)
#   NGPUS: Number of GPUs to use, or "all" for all available (default: all)
#   MODEL_FILE: Simulation model JSON file with component part numbers (default: simulator_inputs/simulation_model/example_simulation_model.json)
# 
# Note: Mission profile is automatically loaded from:
#   - simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv
#   - simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv

set -e  # Exit on error

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default values
ROUNDS="${1:-6}"
NGPUS_ARG="${2:-all}"
MODEL_FILE="${3:-simulator_inputs/simulation_model/example_simulation_model.json}"

# Build the executable if it doesn't exist
if [ ! -f "bin/trace_pv" ]; then
    echo "Building trace_pv..."
    make clean
    make
fi

# Check if model file exists
if [ ! -f "$MODEL_FILE" ]; then
    echo "ERROR: Simulation model file not found: $MODEL_FILE"
    echo "Usage: $0 [ROUNDS] [NGPUS] [MODEL_FILE]"
    exit 1
fi

# Check if mission profile files exist
ENV_CSV="simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv"
OP_CSV="simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv"

if [ ! -f "$ENV_CSV" ]; then
    echo "ERROR: Environmental mission profile not found: $ENV_CSV"
    exit 1
fi

if [ ! -f "$OP_CSV" ]; then
    echo "ERROR: Operating mission profile not found: $OP_CSV"
    exit 1
fi

# Create results directory with timestamp
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RESULTS_DIR="results/results_${TIMESTAMP}"
mkdir -p "$RESULTS_DIR"

# Log file
LOG_FILE="$RESULTS_DIR/run_all_configs.log"

echo "==========================================" | tee -a "$LOG_FILE"
echo "Running All Configurations" | tee -a "$LOG_FILE"
echo "Mission Profile:" | tee -a "$LOG_FILE"
echo "  Environmental: $ENV_CSV" | tee -a "$LOG_FILE"
echo "  Operating: $OP_CSV" | tee -a "$LOG_FILE"
echo "Simulation Model: $MODEL_FILE" | tee -a "$LOG_FILE"
echo "Rounds per Iteration: $ROUNDS" | tee -a "$LOG_FILE"
echo "  (Note: Simulation will run multiple iterations until degradation = 1.0)" | tee -a "$LOG_FILE"
echo "GPUs: $NGPUS_ARG" | tee -a "$LOG_FILE"
echo "Results Directory: $RESULTS_DIR" | tee -a "$LOG_FILE"
echo "Timestamp: $TIMESTAMP" | tee -a "$LOG_FILE"
echo "==========================================" | tee -a "$LOG_FILE"
echo ""

# Topologies and modulations
TOPOLOGIES=("3l2s")
#POLOGIES=("2l2s" "2l1s" "3l2s" "3l1s")
MODULATIONS=("svm")

# Count total configurations
TOTAL_CONFIGS=$((${#TOPOLOGIES[@]} * ${#MODULATIONS[@]}))
CURRENT_CONFIG=0

# Function to run a single configuration
run_config() {
    local topology=$1
    local modulation=$2
    local ngpus=$3
    
    CURRENT_CONFIG=$((CURRENT_CONFIG + 1))
    local config_name="${topology}_${modulation}"
    if [ "$ngpus" != "all" ]; then
        config_name="${config_name}_gpu${ngpus}"
    fi
    
    echo "" | tee -a "$LOG_FILE"
    echo "[$CURRENT_CONFIG/$TOTAL_CONFIGS] Running: $config_name" | tee -a "$LOG_FILE"
    echo "  Topology: $topology, Modulation: $modulation, GPUs: $ngpus" | tee -a "$LOG_FILE"
    echo "  Model: $MODEL_FILE" | tee -a "$LOG_FILE"
    echo "  Started at: $(date)" | tee -a "$LOG_FILE"
    
    local start_time=$(date +%s)
    
    # Build command
    # Note: --rounds now refers to rounds per mission profile iteration
    # The simulation will run multiple iterations until degradation reaches 1.0
    # Mission profile is automatically loaded from fixed paths
    local cmd="./bin/trace_pv --topology $topology --rounds $ROUNDS --modulation $modulation --model $MODEL_FILE"
    if [ "$ngpus" != "all" ]; then
        cmd="$cmd --ngpus $ngpus"
    else
        cmd="$cmd --ngpus all"
    fi
    
    # Run the command and capture output
    local output_file="$RESULTS_DIR/${config_name}.out"
    local error_file="$RESULTS_DIR/${config_name}.err"
    
    if $cmd > "$output_file" 2> "$error_file"; then
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        echo "  ✓ Completed successfully in ${duration}s" | tee -a "$LOG_FILE"
        
        # Extract key metrics from output
        echo "  Key Results:" | tee -a "$LOG_FILE"
        
        # Extract degradation limit information
        if grep -q "DEGRADATION LIMIT REACHED" "$output_file"; then
            echo "  Degradation Status:" | tee -a "$LOG_FILE"
            grep -A 3 "DEGRADATION LIMIT REACHED" "$output_file" | tee -a "$LOG_FILE"
        fi
        
        # Extract failed component
        if grep -q "Failed Component:" "$output_file"; then
            echo "  Failed Component:" | tee -a "$LOG_FILE"
            grep "Failed Component:" "$output_file" | tee -a "$LOG_FILE"
        fi
        
        # Extract mission profile iterations
        if grep -q "Total Mission Profile Iterations:" "$output_file"; then
            echo "  Mission Profile Iterations:" | tee -a "$LOG_FILE"
            grep "Total Mission Profile Iterations:" "$output_file" | tee -a "$LOG_FILE"
        fi
        
        # Extract total rounds
        if grep -q "Total Rounds:" "$output_file"; then
            echo "  Total Rounds:" | tee -a "$LOG_FILE"
            grep "Total Rounds:" "$output_file" | tee -a "$LOG_FILE"
        fi
        
        # Extract execution time
        if grep -q "Execution Time" "$output_file"; then
            echo "  Execution Time:" | tee -a "$LOG_FILE"
            grep "Execution Time" "$output_file" | tee -a "$LOG_FILE"
        fi
        
        # Extract GPU times
        if grep -q "Simulation Time per GPU" "$output_file"; then
            echo "  GPU Times:" | tee -a "$LOG_FILE"
            grep -A 10 "Simulation Time per GPU" "$output_file" | head -5 | tee -a "$LOG_FILE"
        fi
        
        # Extract final degradation progress
        if grep -q "Global Cumulative Degradation Progress" "$output_file"; then
            echo "  Final Degradation Progress:" | tee -a "$LOG_FILE"
            # Get the last occurrence of global cumulative degradation progress
            grep -A 10 "Global Cumulative Degradation Progress" "$output_file" | tail -10 | tee -a "$LOG_FILE"
        fi
    else
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        echo "  ✗ Failed after ${duration}s" | tee -a "$LOG_FILE"
        echo "  Error output:" | tee -a "$LOG_FILE"
        cat "$error_file" | tee -a "$LOG_FILE"
        return 1
    fi
    
    return 0
}

# Run all configurations
SUCCESS_COUNT=0
FAIL_COUNT=0

for topology in "${TOPOLOGIES[@]}"; do
    for modulation in "${MODULATIONS[@]}"; do
        if run_config "$topology" "$modulation" "$NGPUS_ARG"; then
            SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    done
done

# Summary
echo "" | tee -a "$LOG_FILE"
echo "==========================================" | tee -a "$LOG_FILE"
echo "Summary" | tee -a "$LOG_FILE"
echo "==========================================" | tee -a "$LOG_FILE"
echo "Total Configurations: $TOTAL_CONFIGS" | tee -a "$LOG_FILE"
echo "Successful: $SUCCESS_COUNT" | tee -a "$LOG_FILE"
echo "Failed: $FAIL_COUNT" | tee -a "$LOG_FILE"
echo "Results Directory: $RESULTS_DIR" | tee -a "$LOG_FILE"
echo "Completed at: $(date)" | tee -a "$LOG_FILE"

if [ $FAIL_COUNT -eq 0 ]; then
    echo ""
    echo "✓ All configurations completed successfully!"
    exit 0
else
    echo ""
    echo "✗ Some configurations failed. Check $LOG_FILE for details."
    exit 1
fi
