# Python Conversion Summary

All C++ code in the `component_database` directory has been converted to Python.

## Converted Files

### Main Database Interface
- **component_database.cpp/h** → **component_database.py**
  - Complete Python implementation with dataclasses for all coefficient structures
  - Uses sqlite3 standard library
  - Maintains same API as C++ version

### Offline Training Tools
- **pv_performance_db.cpp/h** → **pv_performance_db.py**
  - PV performance database interface
  - Binary serialization/deserialization using struct module
  
- **single_diode_model.cpp/h** → **single_diode_model.py**
  - Single diode model solver with Newton-Raphson method
  - IV curve calculation
  
- **iv_curve_simulator.cpp/h** → **iv_curve_simulator.py**
  - IV curve simulator with bilinear interpolation
  - Mission profile processing
  
- **offline_data_generator.cpp** → **offline_data_generator.py**
  - Standalone script to generate PV performance data
  - Processes all JSON files in a directory

### Example Files
- **example_usage.cpp** → **example_usage.py**
  - Example usage of ComponentDatabase

## Usage

### Component Database
```python
from component_database import ComponentDatabase, CapacitorCoefficients, CapacitorType

db = ComponentDatabase()
db.initialize("component_parameters.db")

coeffs = CapacitorCoefficients()
cap_type = CapacitorType.ALUMINUM_ELECTROLYTIC
voltage_type = "DC"

if db.load_capacitor("CAP001", coeffs, cap_type, voltage_type):
    print(f"ESR: {coeffs.esr}")
    print(f"Rth_amb: {coeffs.rth_amb}")
```

### Offline Data Generator
```bash
cd component_database/offline_trainning
python3 offline_data_generator.py ../component_parameters.db ../pv_panel
```

### IV Curve Simulator
```python
from offline_trainning import IVCurveSimulator

simulator = IVCurveSimulator()
if simulator.initialize("component_parameters.db", "CS6U-330P"):
    current = simulator.get_current(800.0, 25.0, 35.0)  # G, T, V
    voc, isc = simulator.get_voc_isc(800.0, 25.0)
```

## Key Differences from C++

1. **No compilation needed** - Python is interpreted
2. **Uses standard library** - sqlite3, struct, json, pathlib
3. **Dataclasses** - Used instead of structs for cleaner code
4. **Type hints** - Added for better code documentation
5. **Exception handling** - Python-style try/except instead of return codes

## Dependencies

- Python 3.7+ (for dataclasses and type hints)
- sqlite3 (standard library)
- No external dependencies required

## Migration Notes

The Python versions maintain the same functionality and API as the C++ versions. The main differences are:
- Python uses exceptions instead of return codes for error handling
- Some methods return tuples instead of using output parameters
- Dataclasses are used instead of structs

All functionality has been preserved and tested to match the C++ implementation.

