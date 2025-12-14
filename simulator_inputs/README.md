# Simulator Inputs Directory

This directory contains all input files for the TRACE-PV simulator.

## Directory Structure

```
simulator_inputs/
├── mission_profile/          # Mission profile CSV files (temperature, irradiance, etc.)
│   ├── simulation_cases_100k.csv
│   └── year_long_mission_profile_2024.csv
└── simulation_model/         # Simulation model JSON files (component part numbers)
    └── example_simulation_model.json
```

## Mission Profile (`mission_profile/`)

Mission profile files contain time-series data for environmental conditions. Each CSV file should contain:

- **time**: Timestamp or time index
- **ambient_temperature**: Ambient temperature in °C
- **rh**: Relative humidity in %
- **GHI** or **solar_irradiance**: Global Horizontal Irradiance in W/m²
- **ac_voltage**: AC grid voltage (RMS line-to-line) in V

CSV format:
```
time,ambient_temperature,rh,GHI,ac_voltage
2024-01-01 00:00:00,25.0,50.0,0.0,480.0
2024-01-01 01:00:00,24.5,55.0,0.0,480.0
...
```

## Simulation Model (`simulation_model/`)

Simulation model files specify the component part numbers to use for a simulation run. See `simulation_model/README.md` for detailed format specification.

Key components:
- **capacitor**: DC-link capacitor part number
- **power_module**: IGBT power module part number
- **fan_cooling**: Cooling fan part number
- **pcb**: Printed circuit board part number
- **pv_panel**: PV panel part number (required)

## Usage

When running the simulator:

```bash
./bin/trace_pv --topology 2l2s --csv simulator_inputs/mission_profile/simulation_cases_100k.csv
```

The simulator will:
1. Load the simulation model from `simulator_inputs/simulation_model/example_simulation_model.json`
2. Load the mission profile from the specified CSV file
3. Retrieve component parameters from the database based on part numbers
4. Pre-load IV curve data for the PV panel based on mission profile conditions
5. Run the simulation with the specified components and conditions

## Data Flow

```
Simulation Model (part numbers)
    ↓
Component Database (retrieve parameters)
    ↓
Mission Profile (environmental conditions)
    ↓
IV Curve Database (retrieve IV curves for PV panel)
    ↓
GPU Simulation (run electrical/thermal simulation)
```

