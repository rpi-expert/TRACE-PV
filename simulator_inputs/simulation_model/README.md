# Simulation Model Input Files

This directory contains simulation model configuration files that specify the component part numbers used in a simulation run.

## File Format

The simulation model file is a JSON file with the following structure:

```json
{
  "simulation_model": {
    "description": "Description of this simulation model",
    "capacitor": {
      "part_number": "CAP001",
      "description": "Main DC-link capacitor"
    },
    "power_module": {
      "part_number": "IGBT001",
      "description": "IGBT power module for inverter"
    },
    "fan_cooling": {
      "part_number": "FAN001",
      "description": "Cooling fan for thermal management"
    },
    "pcb": {
      "part_number": "PCB001",
      "description": "Printed circuit board"
    },
    "pv_panel": {
      "part_number": "CHSM66M-HC (210)",
      "description": "PV panel for solar energy generation"
    }
  }
}
```

## Component Part Numbers

Each component type requires a `part_number` that must exist in the component database:

- **capacitor**: Part number for the DC-link capacitor (must exist in `component_database/capacitor/`)
- **power_module**: Part number for the IGBT power module (must exist in `component_database/power_module/`)
- **fan_cooling**: Part number for the cooling fan (must exist in `component_database/fan_cooling/`)
- **pcb**: Part number for the PCB (must exist in `component_database/pcb/`)
- **pv_panel**: Part number for the PV panel (must exist in `component_database/pv_system/`)

The `pv_panel` part number is required. Other components are optional but recommended.

## Usage

The simulation model file is automatically loaded by `main.cpp` from:
```
simulator_inputs/simulation_model/example_simulation_model.json
```

You can create additional simulation model files for different component configurations.

## Retrieving Component Parameters

The simulation code automatically:
1. Loads part numbers from the simulation model file
2. Retrieves component parameters from the database using the part numbers
3. Uses these parameters during simulation

Component parameters can be retrieved from the database using:
- `ComponentDatabase::load_capacitor(part_number, coeffs, capacitor_type)`
- `ComponentDatabase::load_fan_cooling(part_number, coeffs)`
- `ComponentDatabase::load_power_module(part_number, coeffs)`
- `ComponentDatabase::load_pcb(part_number, dimensions, coeffs)`

## Example Files

- `example_simulation_model.json`: Example simulation model with standard components

