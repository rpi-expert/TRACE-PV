"""
PV Performance Database Interface
Python version of pv_performance_db.cpp/h
"""

import sqlite3
import struct
from typing import List, Tuple, Optional
from dataclasses import dataclass


@dataclass
class PVPerformancePoint:
    """Single performance point with normalized IV curve."""
    Voc: float  # Open circuit voltage (V)
    Isc: float  # Short circuit current (A)
    V_norm: List[float]  # Normalized voltage points (V/Voc)
    I_norm: List[float]  # Normalized current points (I/Isc)


@dataclass
class PVGridPoint:
    """Grid point with irradiance, temperature, and performance data."""
    irradiance: int  # W/m²
    temperature: int  # °C
    data: PVPerformancePoint


class PVPerformanceDatabase:
    """PV Performance Database for storing and retrieving pre-calculated IV curves."""
    
    def __init__(self):
        self.db: Optional[sqlite3.Connection] = None
    
    def initialize(self, db_path: str) -> bool:
        """Initialize database connection and create schema if needed."""
        try:
            if self.db:
                self.close()
            self.db = sqlite3.connect(db_path)
            return self.create_schema()
        except sqlite3.Error:
            return False
    
    def create_schema(self) -> bool:
        """Create database schema."""
        sql = """
        CREATE TABLE IF NOT EXISTS pv_performance_maps (
            PartNumber TEXT NOT NULL,
            Irradiance INTEGER NOT NULL,
            Temperature INTEGER NOT NULL,
            Voc REAL NOT NULL,
            Isc REAL NOT NULL,
            ShapeData BLOB NOT NULL,
            PRIMARY KEY (PartNumber, Irradiance, Temperature)
        );
        
        CREATE INDEX IF NOT EXISTS idx_partnumber ON pv_performance_maps(PartNumber);
        """
        
        try:
            cursor = self.db.cursor()
            cursor.executescript(sql)
            self.db.commit()
            return True
        except sqlite3.Error:
            return False
    
    def insert_performance_point(
        self,
        part_number: str,
        irradiance: int,
        temperature: int,
        voc: float,
        isc: float,
        shape_data: List[Tuple[float, float]]
    ) -> bool:
        """Insert a performance point into the database."""
        if not self.db:
            return False
        
        # Serialize shape data
        blob = self.serialize_shape_data(shape_data)
        
        sql = """
        INSERT OR REPLACE INTO pv_performance_maps 
        (PartNumber, Irradiance, Temperature, Voc, Isc, ShapeData)
        VALUES (?, ?, ?, ?, ?, ?)
        """
        
        try:
            cursor = self.db.cursor()
            cursor.execute(sql, (part_number, irradiance, temperature, voc, isc, blob))
            self.db.commit()
            return True
        except sqlite3.Error:
            return False
    
    def load_grid(self, part_number: str) -> List[PVGridPoint]:
        """Load all grid points for a specific part number."""
        if not self.db:
            return []
        
        sql = """
        SELECT Irradiance, Temperature, Voc, Isc, ShapeData
        FROM pv_performance_maps
        WHERE PartNumber = ?
        ORDER BY Irradiance, Temperature
        """
        
        grid_points = []
        try:
            cursor = self.db.cursor()
            cursor.execute(sql, (part_number,))
            
            for row in cursor.fetchall():
                irradiance = row[0]
                temperature = row[1]
                voc = row[2]
                isc = row[3]
                blob_data = row[4]
                
                # Deserialize shape data
                shape_data = self.deserialize_shape_data(blob_data)
                
                # Create performance point
                v_norm = [p[0] for p in shape_data]
                i_norm = [p[1] for p in shape_data]
                
                perf_point = PVPerformancePoint(
                    Voc=voc,
                    Isc=isc,
                    V_norm=v_norm,
                    I_norm=i_norm
                )
                
                grid_point = PVGridPoint(
                    irradiance=irradiance,
                    temperature=temperature,
                    data=perf_point
                )
                
                grid_points.append(grid_point)
            
            return grid_points
        except sqlite3.Error:
            return []
    
    def close(self):
        """Close database connection."""
        if self.db:
            self.db.close()
            self.db = None
    
    def is_initialized(self) -> bool:
        """Check if database is initialized."""
        return self.db is not None
    
    @staticmethod
    def serialize_shape_data(shape_data: List[Tuple[float, float]]) -> bytes:
        """Serialize normalized IV curve to binary format."""
        num_points = len(shape_data)
        
        # Format: [num_points (int32)] [V_norm (double)] [I_norm (double)] ...
        blob = struct.pack('i', num_points)
        
        # Write V_norm values
        for v, _ in shape_data:
            blob += struct.pack('d', v)
        
        # Write I_norm values
        for _, i in shape_data:
            blob += struct.pack('d', i)
        
        return blob
    
    @staticmethod
    def deserialize_shape_data(data: bytes) -> List[Tuple[float, float]]:
        """Deserialize binary format to normalized IV curve."""
        if not data or len(data) < 4:
            return []
        
        # Read number of points
        num_points = struct.unpack('i', data[:4])[0]
        
        if num_points < 0 or num_points > 10000:
            return []
        
        expected_size = 4 + num_points * 2 * 8  # int32 + num_points * 2 * double
        if len(data) < expected_size:
            return []
        
        shape_data = []
        offset = 4
        
        # Read V_norm values
        v_norm = []
        for i in range(num_points):
            v = struct.unpack('d', data[offset:offset+8])[0]
            v_norm.append(v)
            offset += 8
        
        # Read I_norm values
        for i in range(num_points):
            i_norm = struct.unpack('d', data[offset:offset+8])[0]
            shape_data.append((v_norm[i], i_norm))
            offset += 8
        
        return shape_data

