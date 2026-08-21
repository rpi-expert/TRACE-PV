# A2S v2 - Batch Simulation System

This project extends the A2S standalone simulation to support batch processing of multiple simulation cases with different environmental conditions.

## Features

- **Batch Processing**: Process multiple simulation cases in batches based on GPU capacity
- **CSV Input**: Read simulation cases from CSV file (ambient temperature, solar irradiance, AC voltage)
- **GPU Capacity Detection**: Automatically calculates optimal batch size based on available GPU resources
- **Round-based Processing**: Process cases in multiple rounds with progress reporting
- **Topology Selection**: Support for four topology types via command-line argument
- **Performance Tracking**: Records simulation time for each case and overall execution

## Topology Types

- `2l2s`: Two-level two-stage
- `2l1s`: Two-level single-stage
- `3l2s`: Three-level two-stage
- `3l1s`: Three-level single-stage

## Input Modes

The current `trace_pv` binary supports two input modes:

- Mission profile mode, using either the split mission files or one combined CSV.
- Static mode, using one fixed temperature/RH/voltage/power set for repeated simulation cases.

## CSV Input Format

The CSV file should have the following format:
```
ambient_temperature,solar_irradiance,ac_voltage
25.0,1000.0,480.0
30.0,800.0,475.0
...
```

Combined mission profiles may also use:

```
time,ambient_temperature,rh,GHI,ac_voltage,ac_power
2024-01-01 07:25:00,2.09,96.38,5.11,259.62,300
```

`ac_power` is expressed in watts with positive values representing power
exported by the inverter to the grid. When it is absent, GHI is not used as an
enclosure heat-load substitute.

- `ambient_temperature`: Temperature in Celsius (typically 15-45°C)
- `solar_irradiance`: Solar irradiance in W/m² (typically 200-1200 W/m²)
- `ac_voltage`: AC voltage RMS line-to-line in Volts (typically 400-500 V)

An example file with 100,000 cases is provided: `simulation_cases_100k.csv`

## Building

```bash
make
```

## Usage

```bash
./trace_pv --topology <2l2s|2l1s|3l2s|3l1s> [--input-mode mission|static] [--mission-csv <file.csv>] [--rounds <N>] [--modulation <svm|spwm>]
```

### Arguments

- `--topology` or `-t`: Topology type (required)
- `--mission-csv`, `--csv`, or `-c`: Path to combined mission profile CSV (optional)
- `--input-mode`: `mission` or `static` (default: `mission`)
- `--static-temp`, `--static-rh`, `--static-voltage`, `--static-power`: Static mode values (defaults: 95, 95, 500, 300)
- `--rounds` or `-r`: Number of rounds to process (default: 1)
- `--modulation` or `-m`: Modulation type svm or spwm (default: svm)

### Examples

```bash
# Process all cases in one round
./trace_pv --topology 2l2s --mission-csv simulator_inputs/mission_profile/year_long_mission_profile_2024.csv

# Run static mode
./trace_pv --topology 3l2s --input-mode static --static-temp 95 --static-rh 95 --static-voltage 500 --static-power 300

# Use SPWM modulation
./trace_pv --topology 3l2s --modulation spwm
```

### Using Make

```bash
make run TOPOLOGY=2l2s CSV=simulation_cases_100k.csv ROUNDS=1 MOD=svm
```

## Output

- Simulation results are saved to `outputs/<topology>/<modulation>/` directory
- Timing logs are saved to `logs/batch_timing_<topology>.log`

## GPU Capacity Calculation

The system automatically:
1. Queries GPU capacity (threads, memory, compute capability)
2. Calculates fundamental cycle size based on switching frequency and fundamental frequency
3. Determines optimal batch size considering:
   - Available GPU memory
   - Maximum thread capacity
   - Memory requirements per simulation case

## PV Voltage Model

PV voltage is calculated from ambient temperature and solar irradiance using a simple model:
- Temperature coefficient: -0.004/°C
- Reference temperature: 25°C
- Reference irradiance: 1000 W/m²
- Base voltage: 500 V

Formula: `V_pv = V_base * (1 + temp_coeff * (T - T_ref)) * (G / G_ref)`

## Performance

The system processes cases in batches to maximize GPU utilization while respecting memory constraints. Progress is reported after each round completes.
