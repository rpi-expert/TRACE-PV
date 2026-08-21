"""
IV Curve Simulator with Bilinear Interpolation
Python version of iv_curve_simulator.cpp/h
"""

from typing import List, Tuple, Optional
from dataclasses import dataclass
try:
    from .pv_performance_db import PVPerformanceDatabase, PVGridPoint
except ImportError:
    from pv_performance_db import PVPerformanceDatabase, PVGridPoint


@dataclass
class MissionProfilePoint:
    """Mission Profile Point."""
    temperature: float  # °C
    irradiance: float  # W/m²


class IVCurveSimulator:
    """IV Curve Simulator with Bilinear Interpolation."""
    
    def __init__(self):
        self.initialized: bool = False
        self.part_number: str = ""
        self.grid_points: List[PVGridPoint] = []
        self.db: PVPerformanceDatabase = PVPerformanceDatabase()
    
    def initialize(self, db_path: str, part_number: str) -> bool:
        """Initialize simulator for a specific part number."""
        if not self.db.initialize(db_path):
            return False
        
        self.grid_points = self.db.load_grid(part_number)
        
        if not self.grid_points:
            return False
        
        self.part_number = part_number
        self.initialized = True
        return True
    
    def _find_surrounding_points(
        self,
        G: float,
        T: float
    ) -> Optional[Tuple[PVGridPoint, PVGridPoint, PVGridPoint, PVGridPoint]]:
        """Find the four surrounding grid points for bilinear interpolation."""
        # Find G bounds
        G_low = int((G // 100) * 100)
        G_high = G_low + 100
        
        # Find T bounds
        T_low = int((T // 5) * 5)
        T_high = T_low + 5
        
        # Clamp to grid bounds
        G_low = max(0, min(G_low, 1000))
        G_high = max(0, min(G_high, 1000))
        T_low = max(0, min(T_low, 60))
        T_high = max(0, min(T_high, 60))
        
        # Find points in grid
        p00 = None  # (G_low, T_low)
        p10 = None  # (G_high, T_low)
        p01 = None  # (G_low, T_high)
        p11 = None  # (G_high, T_high)
        
        for point in self.grid_points:
            if point.irradiance == G_low and point.temperature == T_low:
                p00 = point
            elif point.irradiance == G_high and point.temperature == T_low:
                p10 = point
            elif point.irradiance == G_low and point.temperature == T_high:
                p01 = point
            elif point.irradiance == G_high and point.temperature == T_high:
                p11 = point
        
        # Check if all four points found
        if p00 and p10 and p01 and p11:
            return p00, p10, p01, p11
        return None
    
    def _interpolate_voc(
        self,
        G: float,
        T: float,
        p00: PVGridPoint,
        p10: PVGridPoint,
        p01: PVGridPoint,
        p11: PVGridPoint
    ) -> float:
        """Bilinear interpolation of Voc."""
        G0 = float(p00.irradiance)
        G1 = float(p10.irradiance)
        T0 = float(p00.temperature)
        T1 = float(p01.temperature)
        
        w_G = (G - G0) / (G1 - G0) if (G1 - G0) != 0 else 0
        w_T = (T - T0) / (T1 - T0) if (T1 - T0) != 0 else 0
        
        # Interpolate along G axis first
        voc_G0 = p00.data.Voc * (1.0 - w_G) + p10.data.Voc * w_G
        voc_G1 = p01.data.Voc * (1.0 - w_G) + p11.data.Voc * w_G
        
        # Interpolate along T axis
        voc = voc_G0 * (1.0 - w_T) + voc_G1 * w_T
        
        return voc
    
    def _interpolate_isc(
        self,
        G: float,
        T: float,
        p00: PVGridPoint,
        p10: PVGridPoint,
        p01: PVGridPoint,
        p11: PVGridPoint
    ) -> float:
        """Bilinear interpolation of Isc."""
        G0 = float(p00.irradiance)
        G1 = float(p10.irradiance)
        T0 = float(p00.temperature)
        T1 = float(p01.temperature)
        
        w_G = (G - G0) / (G1 - G0) if (G1 - G0) != 0 else 0
        w_T = (T - T0) / (T1 - T0) if (T1 - T0) != 0 else 0
        
        # Interpolate along G axis first
        isc_G0 = p00.data.Isc * (1.0 - w_G) + p10.data.Isc * w_G
        isc_G1 = p01.data.Isc * (1.0 - w_G) + p11.data.Isc * w_G
        
        # Interpolate along T axis
        isc = isc_G0 * (1.0 - w_T) + isc_G1 * w_T
        
        return isc
    
    def _interpolate_shape(
        self,
        G: float,
        T: float,
        p00: PVGridPoint,
        p10: PVGridPoint,
        p01: PVGridPoint,
        p11: PVGridPoint
    ) -> List[Tuple[float, float]]:
        """Bilinear interpolation of normalized shape data."""
        G0 = float(p00.irradiance)
        G1 = float(p10.irradiance)
        T0 = float(p00.temperature)
        T1 = float(p01.temperature)
        
        w_G = (G - G0) / (G1 - G0) if (G1 - G0) != 0 else 0
        w_T = (T - T0) / (T1 - T0) if (T1 - T0) != 0 else 0
        
        # Find common V_norm points (use the longest one as reference)
        max_size = max(
            len(p00.data.V_norm),
            len(p10.data.V_norm),
            len(p01.data.V_norm),
            len(p11.data.V_norm)
        )
        
        # Use p00's V_norm as reference
        V_norm_ref = p00.data.V_norm
        
        interpolated_shape = []
        
        # For each normalized voltage point, interpolate the normalized current
        for i in range(len(V_norm_ref)):
            V_norm = V_norm_ref[i]
            
            # Get I_norm from each corner (with bounds checking)
            I00 = p00.data.I_norm[i] if i < len(p00.data.I_norm) else 0.0
            I10 = p10.data.I_norm[i] if i < len(p10.data.I_norm) else 0.0
            I01 = p01.data.I_norm[i] if i < len(p01.data.I_norm) else 0.0
            I11 = p11.data.I_norm[i] if i < len(p11.data.I_norm) else 0.0
            
            # Bilinear interpolation
            I_G0 = I00 * (1.0 - w_G) + I10 * w_G
            I_G1 = I01 * (1.0 - w_G) + I11 * w_G
            I_norm = I_G0 * (1.0 - w_T) + I_G1 * w_T
            
            interpolated_shape.append((V_norm, I_norm))
        
        return interpolated_shape
    
    def _interpolate_normalized_current(
        self,
        V_norm: float,
        shape_data: List[Tuple[float, float]]
    ) -> float:
        """Interpolate normalized current at a normalized voltage."""
        if not shape_data:
            return 0.0
        
        # Clamp V_norm to valid range
        V_norm = max(0.0, min(1.0, V_norm))
        
        # Find surrounding points for linear interpolation
        if V_norm <= shape_data[0][0]:
            return shape_data[0][1]
        if V_norm >= shape_data[-1][0]:
            return shape_data[-1][1]
        
        # Linear interpolation
        for i in range(len(shape_data) - 1):
            if shape_data[i][0] <= V_norm <= shape_data[i + 1][0]:
                V0, I0 = shape_data[i]
                V1, I1 = shape_data[i + 1]
                
                w = (V_norm - V0) / (V1 - V0) if (V1 - V0) != 0 else 0
                return I0 * (1.0 - w) + I1 * w
        
        return 0.0
    
    def get_iv_curve(
        self,
        G: float,
        T: float
    ) -> Tuple[List[float], List[float]]:
        """Get IV curve for a specific condition using bilinear interpolation."""
        if not self.initialized:
            return [], []
        
        points = self._find_surrounding_points(G, T)
        if not points:
            return [], []
        
        p00, p10, p01, p11 = points
        
        # Interpolate Voc and Isc
        voc = self._interpolate_voc(G, T, p00, p10, p01, p11)
        isc = self._interpolate_isc(G, T, p00, p10, p01, p11)
        
        # Interpolate normalized shape
        shape = self._interpolate_shape(G, T, p00, p10, p01, p11)
        
        # Denormalize to get actual IV curve
        voltage_points = [p[0] * voc for p in shape]
        current_points = [p[1] * isc for p in shape]
        
        return voltage_points, current_points
    
    def get_current(self, G: float, T: float, V: float) -> float:
        """Get current at a specific voltage for given conditions."""
        if not self.initialized:
            return -1.0
        
        points = self._find_surrounding_points(G, T)
        if not points:
            return -1.0
        
        p00, p10, p01, p11 = points
        
        # Interpolate Voc and Isc
        voc = self._interpolate_voc(G, T, p00, p10, p01, p11)
        isc = self._interpolate_isc(G, T, p00, p10, p01, p11)
        
        # Normalize voltage
        V_norm = V / voc if voc > 0 else 0
        
        # Interpolate normalized shape
        shape = self._interpolate_shape(G, T, p00, p10, p01, p11)
        
        # Get normalized current
        I_norm = self._interpolate_normalized_current(V_norm, shape)
        
        # Denormalize
        return I_norm * isc
    
    def get_voc_isc(self, G: float, T: float) -> Tuple[bool, float, float]:
        """Get Voc and Isc for given conditions."""
        if not self.initialized:
            return False, 0.0, 0.0
        
        points = self._find_surrounding_points(G, T)
        if not points:
            return False, 0.0, 0.0
        
        p00, p10, p01, p11 = points
        
        voc = self._interpolate_voc(G, T, p00, p10, p01, p11)
        isc = self._interpolate_isc(G, T, p00, p10, p01, p11)
        
        return True, voc, isc
    
    def process_mission_profile(
        self,
        mission_profile: List[MissionProfilePoint],
        voltage_requested: List[float]
    ) -> Tuple[bool, List[float]]:
        """Process a mission profile (time-series of conditions)."""
        if not self.initialized:
            return False, []
        
        if len(mission_profile) != len(voltage_requested):
            return False, []
        
        current_output = []
        
        for i, point in enumerate(mission_profile):
            I = self.get_current(point.irradiance, point.temperature, voltage_requested[i])
            
            if I < 0:
                return False, []
            
            current_output.append(I)
        
        return True, current_output
    
    def is_initialized(self) -> bool:
        """Check if simulator is initialized."""
        return self.initialized

