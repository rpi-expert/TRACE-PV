"""
Component database interface for loading parameters from SQLite database.
Python version of component_database.cpp/h
"""

import sqlite3
import json
from typing import Optional, Dict, List, Tuple
from enum import Enum
from dataclasses import dataclass


class CapacitorType(Enum):
    ALUMINUM_ELECTROLYTIC = "ALUMINUM_ELECTROLYTIC"
    TANTALUM = "TANTALUM"
    CERAMIC = "CERAMIC"
    FILM = "FILM"


@dataclass
class FilmCapacitorCoefficients:
    A: float = 0.0
    n: float = 0.0
    Ea: float = 0.0
    beta: float = 0.0
    V_rated: float = 0.0
    T_ref: float = 0.0
    RH_ref: float = 0.0


@dataclass
class AluminumElectrolyticCoefficients:
    L0: float = 0.0
    V0: float = 0.0
    T0: float = 0.0
    beta_min: float = 0.0
    beta_max: float = 0.0


@dataclass
class CapacitorCoefficients:
    type: CapacitorType = CapacitorType.ALUMINUM_ELECTROLYTIC
    film: Optional[FilmCapacitorCoefficients] = None
    aluminum: Optional[AluminumElectrolyticCoefficients] = None
    rth_amb: float = 0.5
    rth_surf: float = 0.3
    esr: float = 0.01
    capacitance: float = 330e-6


@dataclass
class FanElectricalCoefficients:
    A: float = 0.0
    n: float = 0.0
    Ea: float = 0.0


@dataclass
class FanMechanicalCoefficients:
    A: float = 0.0
    c: float = 0.0


@dataclass
class FanCoefficients:
    electrical: FanElectricalCoefficients = None
    mechanical: FanMechanicalCoefficients = None
    
    def __post_init__(self):
        if self.electrical is None:
            self.electrical = FanElectricalCoefficients()
        if self.mechanical is None:
            self.mechanical = FanMechanicalCoefficients()


@dataclass
class PowerModuleDeltaTCoefficients:
    A: float = 0.0
    n: float = 0.0
    Ea: float = 0.0


@dataclass
class PowerModuleArrheniusCoefficients:
    A: float = 0.0
    n1: float = 0.0
    n2: float = 0.0
    Ea: float = 0.0
    RH_ref: float = 95.0
    T_ref: float = 348.15
    V_ref: float = 800.0


@dataclass
class PowerModuleCoefficients:
    deltaT_model: PowerModuleDeltaTCoefficients = None
    arrhenius_model: PowerModuleArrheniusCoefficients = None
    
    def __post_init__(self):
        if self.deltaT_model is None:
            self.deltaT_model = PowerModuleDeltaTCoefficients()
        if self.arrhenius_model is None:
            self.arrhenius_model = PowerModuleArrheniusCoefficients()


@dataclass
class SolderJointCoefficients:
    A: float = 0.0
    B: float = 0.0
    C: float = 0.0
    D: float = 0.0
    Ea: float = 0.0
    T_ref: float = 0.0


@dataclass
class PCBDimensions:
    length: float = 0.0
    width: float = 0.0
    thickness: float = 0.0


class ComponentDatabase:
    """Component database interface for loading parameters from SQLite database."""
    
    def __init__(self):
        self.db_handle: Optional[sqlite3.Connection] = None
        self.capacitor_coeffs: Dict[str, CapacitorCoefficients] = {}
        self.capacitor_types: Dict[str, CapacitorType] = {}
        self.capacitor_voltage_types: Dict[str, str] = {}
        self.fan_coeffs: Dict[str, FanCoefficients] = {}
        self.power_module_coeffs: Dict[str, PowerModuleCoefficients] = {}
        self.pcb_dimensions: Dict[str, PCBDimensions] = {}
        self.pcb_coeffs: Dict[str, SolderJointCoefficients] = {}
    
    def initialize(self, db_path: str) -> bool:
        """Initialize database connection."""
        try:
            self.db_handle = sqlite3.connect(db_path)
            return True
        except sqlite3.Error as e:
            print(f"Cannot open database: {e}")
            return False
    
    def __del__(self):
        """Close database connection on destruction."""
        if self.db_handle:
            self.db_handle.close()
    
    def _parse_capacitor_type(self, type_str: str) -> CapacitorType:
        """Parse capacitor type string to enum."""
        type_map = {
            "ALUMINUM_ELECTROLYTIC": CapacitorType.ALUMINUM_ELECTROLYTIC,
            "TANTALUM": CapacitorType.TANTALUM,
            "CERAMIC": CapacitorType.CERAMIC,
            "FILM": CapacitorType.FILM
        }
        return type_map.get(type_str, CapacitorType.ALUMINUM_ELECTROLYTIC)
    
    def load_capacitor(self, part_number: str, coeffs: CapacitorCoefficients,
                      capacitor_type: CapacitorType, voltage_type: str) -> bool:
        """Load capacitor parameters by part number."""
        if not self.db_handle:
            return False
        
        sql = """SELECT capacitor_type, voltage_type, A, n, Ea, beta, V_rated, T_ref, RH_ref,
                 L0, V0, T0, beta_min, beta_max, rth_amb, rth_surf, esr, capacitance
                 FROM capacitor WHERE part_number = ?"""
        
        try:
            cursor = self.db_handle.cursor()
            cursor.execute(sql, (part_number,))
            row = cursor.fetchone()
            
            if row:
                type_str = row[0]
                voltage_type_str = row[1] or "DC"
                capacitor_type = self._parse_capacitor_type(type_str)
                coeffs.type = capacitor_type
                voltage_type = voltage_type_str
                
                if capacitor_type == CapacitorType.FILM:
                    coeffs.film = FilmCapacitorCoefficients(
                        A=row[2] or 0.0,
                        n=row[3] or 0.0,
                        Ea=row[4] or 0.0,
                        beta=row[5] or 0.0,
                        V_rated=row[6] or 0.0,
                        T_ref=row[7] or 0.0,
                        RH_ref=row[8] or 0.0
                    )
                elif capacitor_type == CapacitorType.ALUMINUM_ELECTROLYTIC:
                    coeffs.aluminum = AluminumElectrolyticCoefficients(
                        L0=row[9] or 0.0,
                        V0=row[10] or 0.0,
                        T0=row[11] or 0.0,
                        beta_min=row[12] or 0.0,
                        beta_max=row[13] or 0.0
                    )
                
                # Load thermal and electrical parameters
                coeffs.rth_amb = row[14] if row[14] is not None else 0.5
                coeffs.rth_surf = row[15] if row[15] is not None else 0.3
                coeffs.esr = row[16] if row[16] is not None else 0.01
                coeffs.capacitance = row[17] if row[17] is not None else 330e-6
                
                return True
            return False
        except sqlite3.Error as e:
            print(f"Failed to load capacitor: {e}")
            return False
    
    def load_fan_cooling(self, part_number: str, coeffs: FanCoefficients) -> bool:
        """Load fan cooling parameters by part number."""
        if not self.db_handle:
            return False
        
        sql = "SELECT A_electrical, n, Ea, A_mechanical, c FROM fan_cooling WHERE part_number = ?"
        
        try:
            cursor = self.db_handle.cursor()
            cursor.execute(sql, (part_number,))
            row = cursor.fetchone()
            
            if row:
                coeffs.electrical = FanElectricalCoefficients(
                    A=row[0] or 0.0,
                    n=row[1] or 0.0,
                    Ea=row[2] or 0.0
                )
                coeffs.mechanical = FanMechanicalCoefficients(
                    A=row[3] or 0.0,
                    c=row[4] or 0.0
                )
                return True
            return False
        except sqlite3.Error as e:
            print(f"Failed to load fan cooling: {e}")
            return False
    
    def load_power_module(self, part_number: str, coeffs: PowerModuleCoefficients) -> bool:
        """Load power module parameters by part number."""
        if not self.db_handle:
            return False
        
        sql = "SELECT deltaT_A, deltaT_n, deltaT_Ea, arrhenius_A, arrhenius_n1, arrhenius_n2, arrhenius_Ea, arrhenius_RH_ref, arrhenius_T_ref, arrhenius_V_ref FROM power_module WHERE part_number = ?"
        
        try:
            cursor = self.db_handle.cursor()
            cursor.execute(sql, (part_number,))
            row = cursor.fetchone()
            
            if row:
                # Load deltaT model coefficients
                coeffs.deltaT_model.A = row[0] or 0.0
                coeffs.deltaT_model.n = row[1] or 0.0
                coeffs.deltaT_model.Ea = row[2] or 0.0
                # Load Arrhenius model coefficients
                coeffs.arrhenius_model.A = row[3] or 0.0
                coeffs.arrhenius_model.n1 = row[4] or 0.0
                coeffs.arrhenius_model.n2 = row[5] or 0.0
                coeffs.arrhenius_model.Ea = row[6] or 0.0
                coeffs.arrhenius_model.RH_ref = row[7] or 95.0
                coeffs.arrhenius_model.T_ref = row[8] or 348.15
                coeffs.arrhenius_model.V_ref = row[9] or 800.0
                return True
            return False
        except sqlite3.Error as e:
            print(f"Failed to load power module: {e}")
            return False
    
    def load_pcb(self, part_number: str, dimensions: PCBDimensions,
                 coeffs: SolderJointCoefficients) -> bool:
        """Load PCB parameters by part number."""
        if not self.db_handle:
            return False
        
        sql = "SELECT length, width, thickness, A, B, C, D, Ea, T_ref FROM pcb WHERE part_number = ?"
        
        try:
            cursor = self.db_handle.cursor()
            cursor.execute(sql, (part_number,))
            row = cursor.fetchone()
            
            if row:
                dimensions.length = row[0] or 0.0
                dimensions.width = row[1] or 0.0
                dimensions.thickness = row[2] or 0.0
                coeffs.A = row[3] or 0.0
                coeffs.B = row[4] or 0.0
                coeffs.C = row[5] or 0.0
                coeffs.D = row[6] or 0.0
                coeffs.Ea = row[7] or 0.0
                coeffs.T_ref = row[8] or 0.0
                return True
            return False
        except sqlite3.Error as e:
            print(f"Failed to load PCB: {e}")
            return False
    
    def load_from_input_file(self, input_file: str) -> Tuple[List[str], List[str], List[str], List[str]]:
        """Load component part numbers from input file (JSON format)."""
        try:
            with open(input_file, 'r') as f:
                data = json.load(f)
            
            # Extract component arrays from JSON
            capacitor_parts = data.get("capacitor", [])
            fan_parts = data.get("fan_cooling", [])
            power_module_parts = data.get("power_module", [])
            pcb_parts = data.get("pcb", [])
            
            return capacitor_parts, fan_parts, power_module_parts, pcb_parts
        except Exception as e:
            print(f"Cannot open input file: {input_file}, error: {e}")
            return [], [], [], []
    
    def get_pv_panel_parts(self, input_file: str) -> List[str]:
        """Extract PV panel part numbers from input file."""
        try:
            with open(input_file, 'r') as f:
                data = json.load(f)
            return data.get("pv_panel", [])
        except Exception as e:
            print(f"Cannot open input file: {input_file}, error: {e}")
            return []
    
    def preload_all_components(self, input_file: str) -> bool:
        """Pre-load all component parameters for simulation."""
        capacitor_parts, fan_parts, power_module_parts, pcb_parts = \
            self.load_from_input_file(input_file)
        
        # Load capacitor components
        for part_number in capacitor_parts:
            coeffs = CapacitorCoefficients()
            cap_type = CapacitorType.ALUMINUM_ELECTROLYTIC
            voltage_type = "DC"
            if self.load_capacitor(part_number, coeffs, cap_type, voltage_type):
                self.capacitor_coeffs[part_number] = coeffs
                self.capacitor_types[part_number] = cap_type
                self.capacitor_voltage_types[part_number] = voltage_type
            else:
                print(f"Warning: Cannot load capacitor {part_number}")
        
        # Load fan cooling components
        for part_number in fan_parts:
            coeffs = FanCoefficients()
            if self.load_fan_cooling(part_number, coeffs):
                self.fan_coeffs[part_number] = coeffs
            else:
                print(f"Warning: Cannot load fan cooling {part_number}")
        
        # Load power module components
        for part_number in power_module_parts:
            coeffs = PowerModuleCoefficients()
            if self.load_power_module(part_number, coeffs):
                self.power_module_coeffs[part_number] = coeffs
            else:
                print(f"Warning: Cannot load power module {part_number}")
        
        # Load PCB components
        for part_number in pcb_parts:
            dims = PCBDimensions()
            coeffs = SolderJointCoefficients()
            if self.load_pcb(part_number, dims, coeffs):
                self.pcb_dimensions[part_number] = dims
                self.pcb_coeffs[part_number] = coeffs
            else:
                print(f"Warning: Cannot load PCB {part_number}")
        
        return True
    
    def get_capacitor(self, part_number: str) -> Optional[CapacitorCoefficients]:
        """Get pre-loaded capacitor parameters."""
        return self.capacitor_coeffs.get(part_number)
    
    def get_capacitor_type(self, part_number: str) -> CapacitorType:
        """Get pre-loaded capacitor type."""
        return self.capacitor_types.get(part_number, CapacitorType.ALUMINUM_ELECTROLYTIC)
    
    def get_capacitor_voltage_type(self, part_number: str) -> str:
        """Get pre-loaded capacitor voltage type."""
        return self.capacitor_voltage_types.get(part_number, "DC")
    
    def get_fan_cooling(self, part_number: str) -> Optional[FanCoefficients]:
        """Get pre-loaded fan cooling parameters."""
        return self.fan_coeffs.get(part_number)
    
    def get_power_module(self, part_number: str) -> Optional[PowerModuleCoefficients]:
        """Get pre-loaded power module parameters."""
        return self.power_module_coeffs.get(part_number)
    
    def get_pcb_dimensions(self, part_number: str) -> Optional[PCBDimensions]:
        """Get pre-loaded PCB dimensions."""
        return self.pcb_dimensions.get(part_number)
    
    def get_pcb_coefficients(self, part_number: str) -> Optional[SolderJointCoefficients]:
        """Get pre-loaded PCB coefficients."""
        return self.pcb_coeffs.get(part_number)
    
    def is_initialized(self) -> bool:
        """Check if database is initialized."""
        return self.db_handle is not None

