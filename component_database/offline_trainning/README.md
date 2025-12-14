# PV Performance Database System

A high-performance hybrid simulation architecture for photovoltaic (PV) systems that uses a pre-calculated lookup database with bilinear interpolation to accelerate IV curve generation.

**Note**: The database is now managed in `component_database/`. The PV performance table is stored in the same database file as component parameters (`component_parameters.db`). See `component_database/README.md` for database initialization instructions.

## Architecture Overview

The system consists of three main components:

1. **Database Schema**: SQLite database storing normalized IV curve data
2. **Offline Data Generator**: Populates the database with pre-calculated IV curves
3. **Runtime Simulator**: Fast IV curve retrieval using bilinear interpolation

## Components

### 1. Database Schema (`pv_performance_db.h/cpp`)

SQLite database with the following schema:

```sql
CREATE TABLE pv_performance_maps (
    PartNumber TEXT NOT NULL,
    Irradiance INTEGER NOT NULL,
    Temperature INTEGER NOT NULL,
    Voc REAL NOT NULL,
    Isc REAL NOT NULL,
    ShapeData BLOB NOT NULL,
    PRIMARY KEY (PartNumber, Irradiance, Temperature)
);
```

**Key Features:**
- Primary key: (PartNumber, Irradiance, Temperature)
- Stores Voc, Isc, and normalized shape data (V_norm, I_norm)
- Efficient binary serialization of normalized curves

### 2. Single Diode Model (`single_diode_model.h/cpp`)

Implements the Single Diode Model with Newton-Raphson solver:

**Equation:**
```
I = IL - I0*(exp((V+I*Rs)/(n*Vt)) - 1) - (V+I*Rs)/Rsh
```

**Features:**
- Temperature and irradiance adjustment
- Newton-Raphson iterative solver
- Generates full IV curves with configurable point density

### 3. Offline Data Generator (`offline_data_generator.cpp`)

**Purpose:** Pre-calculate IV curves for a grid of conditions and store in database.

**Grid Definition:**
- Irradiance: 0 to 1000 W/m² (Step: 100)
- Temperature: 0 to 60 °C (Step: 5)
- Total: 11 × 13 = 143 points per panel

**Usage:**
```bash
./offline_data_generator <database_path> <pv_system_directory>
```

**Example:**
```bash
# Using the merged component database location:
./offline_data_generator ../../component_database/component_parameters.db ../../component_database/pv_system
```

### 4. Runtime Simulator (`iv_curve_simulator.h/cpp`)

**Purpose:** Fast IV curve retrieval using bilinear interpolation.

**Key Optimization:**
- Loads entire grid for a PartNumber into memory once
- No database queries during simulation
- Bilinear interpolation for any (T, G) condition

**Bilinear Interpolation:**
For a point (G, T) between grid points:
- Find 4 surrounding points: (G₀,T₀), (G₁,T₀), (G₀,T₁), (G₁,T₁)
- Interpolate Voc, Isc, and normalized shape data
- Denormalize to get actual IV curve

**Usage:**
```cpp
IVCurveSimulator simulator;
simulator.initialize("pv_performance.db", "CHSM66M-HC (210)");

// Get current at specific voltage
double I = simulator.get_current(650.0, 25.0, 30.0);

// Get full IV curve
std::vector<double> V, I;
simulator.get_iv_curve(650.0, 25.0, V, I);
```

### 5. Case Study Example (`case_study_example.cpp`)

Demonstrates the complete interpolation process for:
- **Input:** G=650 W/m², T=25°C, V=30V
- **Grid Points:** G=600, G=700 (both at T=25°C)
- **Output:** Detailed step-by-step calculation

**Usage:**
```bash
# Using the merged component database location:
./case_study_example ../../component_database/component_parameters.db "CHSM66M-HC (210)"
```

## Database Setup

Before generating PV performance data, ensure the database is initialized:

1. **Initialize component database** (includes PV performance table):
   ```bash
   cd component_database
   python3 init_database.py
   ```

2. **Or initialize PV performance table only** (if database already exists):
   ```bash
   cd component_database
   python3 init_pv_performance_table.py
   ```

3. **Generate PV performance data**:
   ```bash
   cd src/simulation_preparations/offline_trainning
   make
   ./offline_data_generator ../../component_database/component_parameters.db ../../component_database/pv_system
   ```

## Building

### Prerequisites

- C++17 compiler (g++ or clang++)
- SQLite3 development libraries
- nlohmann/json header (or modify includes)

### Build Commands

```bash
cd pv_system
make
```

### Manual Compilation

```bash
# Offline generator
g++ -std=c++17 -O2 -I. -I../src offline_data_generator.cpp \
    pv_performance_db.cpp single_diode_model.cpp \
    -lsqlite3 -o offline_data_generator

# Case study
g++ -std=c++17 -O2 -I. -I../src case_study_example.cpp \
    iv_curve_simulator.cpp pv_performance_db.cpp \
    -lsqlite3 -o case_study_example
```

## Case Study: G=650 Interpolation

### Scenario
- **Available Grid Points:** G=600, G=700 (both at T=25°C)
- **Input Condition:** G=650 W/m², T=25°C
- **Task:** Calculate Current at V=30V

### Mathematical Process

#### Step 1: Interpolate Voc
```
w_G = (G - G₀) / (G₁ - G₀) = (650 - 600) / (700 - 600) = 0.5

Voc_interp = Voc_600 × (1 - w_G) + Voc_700 × w_G
           = Voc_600 × 0.5 + Voc_700 × 0.5
```

#### Step 2: Interpolate Isc
```
Isc_interp = Isc_600 × (1 - w_G) + Isc_700 × w_G
           = Isc_600 × 0.5 + Isc_700 × 0.5
```

#### Step 3: Normalize Voltage
```
V_norm = V / Voc_interp = 30 / Voc_interp
```

#### Step 4: Interpolate Normalized Shape
For each normalized voltage point:
```
I_norm_interp = I_norm_600 × (1 - w_G) + I_norm_700 × w_G
```

#### Step 5: Denormalize
```
I = I_norm_interp × Isc_interp
```

### Example Calculation

Assuming:
- Voc_600 = 42.0 V, Voc_700 = 44.0 V
- Isc_600 = 11.0 A, Isc_700 = 12.8 A
- At V_norm = 0.7, I_norm_600 = 0.5, I_norm_700 = 0.55

Then:
```
Voc_interp = 42.0 × 0.5 + 44.0 × 0.5 = 43.0 V
Isc_interp = 11.0 × 0.5 + 12.8 × 0.5 = 11.9 A
V_norm = 30 / 43.0 = 0.698
I_norm_interp = 0.5 × 0.5 + 0.55 × 0.5 = 0.525
I = 0.525 × 11.9 = 6.248 A
```

## File Structure

```
pv_system/
├── README.md                    # This file
├── Makefile                     # Build configuration
├── pv_performance_db.h/cpp      # Database interface
├── single_diode_model.h/cpp     # Single Diode Model solver
├── offline_data_generator.cpp   # Database populator
├── iv_curve_simulator.h/cpp     # Runtime simulator
└── case_study_example.cpp       # Demonstration example
```

## Performance Benefits

1. **Pre-calculation:** IV curves computed once offline
2. **Memory-based lookup:** Grid loaded once, no database I/O during simulation
3. **Fast interpolation:** O(1) lookup + O(n) interpolation (n = curve points)
4. **Scalability:** Handles thousands of mission profile points efficiently

## Integration with TRACE-PV

To integrate with the main TRACE-PV simulator:

```cpp
#include "pv_system/iv_curve_simulator.h"

// Initialize once at simulation start
IVCurveSimulator pv_sim;
pv_sim.initialize("pv_performance.db", part_number);

// Use during simulation loop
for (const auto& profile_point : mission_profile) {
    double I = pv_sim.get_current(
        profile_point.irradiance,
        profile_point.temperature,
        requested_voltage
    );
    // Use I in simulation...
}
```

## Notes

- The normalized shape data captures the "knee" of the IV curve efficiently
- Bilinear interpolation provides smooth transitions between grid points
- The system handles edge cases (outside grid bounds) gracefully
- Database can be pre-populated for all panels in the system

