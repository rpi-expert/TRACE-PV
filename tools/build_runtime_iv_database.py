#!/usr/bin/env python3
"""Generate per-module curves from the component's single-diode parameters.

Writes a separate runtime database; never overwrites the original component DB.
Requires numpy/scipy. Array scaling belongs to the runtime model configuration.
"""
import argparse
import json
import sqlite3
import struct
from pathlib import Path
import numpy as np
from scipy.optimize import brentq
from scipy.special import wrightomega


def curve(panel, irradiance, temperature):
    kelvin = temperature + 273.15
    reference = 298.15
    q, k = 1.602176634e-19, 1.380649e-23
    ideality = panel['DI_factor']
    # Crucial: thermal voltage is for all series-connected cells in ONE module.
    a = panel['Size (cells)'] * ideality * k * kelvin / q
    il = (panel['IL'] + panel['Tk Isc (%/°C)'] / 100 * panel['Isc (A)'] * (kelvin-reference)) * irradiance / 1000
    i0 = panel['I0'] * (kelvin/reference)**3 * np.exp(q*1.121/(ideality*k)*(1/reference-1/kelvin))
    rs = panel['Rs']
    rsh = panel['Rsh'] * 1000 / irradiance
    denominator = 1 + rs/rsh

    def current(voltage):
        log_arg = np.log(rs*i0/(a*denominator)) + (rs*(il+i0)+voltage)/(a*denominator)
        return (il+i0-voltage/rsh)/denominator - a/rs*wrightomega(log_arg)

    voc = brentq(current, 0, a*np.log1p(il/i0)*1.01)
    isc = float(current(0))
    volts = np.linspace(0, voc, 401)
    amps = np.maximum(current(volts), 0)
    amps[-1] = 0
    blob = struct.pack('<i', len(volts)) + (volts/voc).astype('<f8').tobytes() + (amps/isc).astype('<f8').tobytes()
    return voc, isc, blob, float(np.max(volts*amps))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--panel', default='component_database/pv_panel/CS6U-330P.json')
    parser.add_argument('--output', default='component_database/runtime_iv_curves.db')
    args = parser.parse_args()
    panel = json.loads(Path(args.panel).read_text())['PVpanel']
    voc, isc, _, pmax = curve(panel, 1000, 25)
    if abs(voc/panel['Voc (V)']-1)>0.05 or abs(pmax/panel['Pmax (W)']-1)>0.05:
        raise ValueError(f'STC validation failed: Voc={voc}, Pmax={pmax}')
    rows=[]
    for g in [0.01, 1, 10, 25, 50] + list(range(100,1601,50)):
        for t in range(-45,106,5):
            v,i,blob,_ = curve(panel,g,t)
            rows.append((panel['Part Number'],g,t,v,i,blob))
    with sqlite3.connect(args.output) as db:
        db.execute('CREATE TABLE IF NOT EXISTS pv_performance_maps (PartNumber TEXT,Irradiance REAL,Temperature REAL,Voc REAL,Isc REAL,ShapeData BLOB,PRIMARY KEY(PartNumber,Irradiance,Temperature))')
        db.execute('DELETE FROM pv_performance_maps WHERE PartNumber=?',(panel['Part Number'],))
        db.executemany('INSERT INTO pv_performance_maps VALUES (?,?,?,?,?,?)',rows)
    print(f'{args.output}: {len(rows)} curves; module STC Voc={voc:.4f} V, Isc={isc:.4f} A, Pmax={pmax:.4f} W')


if __name__ == '__main__':
    main()
