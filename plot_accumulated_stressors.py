#!/usr/bin/env python3
"""
Plot accumulated stressors for each failure mechanism on each component.

This script reads stressor CSV files and creates visualizations showing
the accumulated stress values for each failure mechanism across components.
"""

try:
    import pandas as pd
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError as e:
    print("Error: Required packages not installed.")
    print("Please install required packages using:")
    print("  pip install pandas matplotlib numpy")
    print("\nOr install from requirements file:")
    print("  pip install -r requirements_plotting.txt")
    raise

from pathlib import Path
import glob
import argparse
from collections import defaultdict

def parse_stressor_column(column_name):
    """
    Parse column name to extract component and failure mechanism.
    
    Examples:
    - 'fan_electrical_external' -> ('fan', 'electrical_external')
    - 'capacitor' -> ('capacitor', None)
    - 'igbt_deltaT' -> ('igbt', 'deltaT')
    """
    parts = column_name.split('_')
    
    # Special cases
    if column_name == 'capacitor':
        return ('capacitor', None)
    elif column_name == 'pcb':
        return ('pcb', None)
    elif column_name.startswith('fan_'):
        # fan_electrical_external, fan_electrical_internal, etc.
        mechanism = '_'.join(parts[1:])
        return ('fan', mechanism)
    elif column_name.startswith('igbt_'):
        # igbt_deltaT, igbt_arrhenius
        mechanism = '_'.join(parts[1:])
        return ('igbt', mechanism)
    else:
        return (column_name, None)

def load_stressor_files(file_pattern=None, results_dir=None):
    """
    Load all stressor CSV files matching the pattern.
    
    Args:
        file_pattern: Glob pattern for files (e.g., 'stressor_round_*.csv')
        results_dir: Directory containing the CSV files
    
    Returns:
        Dictionary mapping file paths to DataFrames
    """
    if results_dir is None:
        results_dir = Path(__file__).parent / 'results'
    else:
        results_dir = Path(results_dir)
    
    if file_pattern is None:
        file_pattern = 'stressor_round_*.csv'
    
    files = sorted(results_dir.glob(file_pattern))
    
    if not files:
        raise FileNotFoundError(f"No files found matching pattern '{file_pattern}' in {results_dir}")
    
    data = {}
    for file_path in files:
        print(f"Loading {file_path.name}...")
        df = pd.read_csv(file_path)
        data[file_path] = df
    
    return data

def accumulate_stressors(data_dict):
    """
    Accumulate stressors across all files for each component and mechanism.
    
    Args:
        data_dict: Dictionary mapping file paths to DataFrames
    
    Returns:
        Dictionary with structure: {component: {mechanism: accumulated_value}}
    """
    accumulated = defaultdict(lambda: defaultdict(float))
    
    for file_path, df in data_dict.items():
        # Skip case_index column
        stressor_cols = [col for col in df.columns if col != 'case_index']
        
        for col in stressor_cols:
            component, mechanism = parse_stressor_column(col)
            
            # Sum all values in this column across all rows
            total = df[col].sum()
            
            if mechanism:
                accumulated[component][mechanism] += total
            else:
                # For components without explicit mechanism, use component name as mechanism
                accumulated[component][component] += total
    
    return accumulated

def plot_accumulated_stressors(accumulated_data, output_file=None, figsize=(14, 8)):
    """
    Create visualizations of accumulated stressors.
    
    Args:
        accumulated_data: Dictionary from accumulate_stressors()
        output_file: Path to save the plot (optional)
        figsize: Figure size tuple
    """
    # Prepare data for plotting
    components = sorted(accumulated_data.keys())
    
    # Create figure with subplots
    fig, axes = plt.subplots(2, 2, figsize=figsize)
    fig.suptitle('Accumulated Stressors by Component and Failure Mechanism', 
                 fontsize=16, fontweight='bold')
    
    # Flatten axes for easier iteration
    axes_flat = axes.flatten()
    
    # Plot 1: Bar chart - Total stressors per component
    ax1 = axes_flat[0]
    component_totals = {comp: sum(mech.values()) for comp, mech in accumulated_data.items()}
    comps = list(component_totals.keys())
    totals = list(component_totals.values())
    
    bars1 = ax1.bar(comps, totals, color='steelblue', alpha=0.7)
    ax1.set_title('Total Accumulated Stressors per Component', fontweight='bold')
    ax1.set_ylabel('Accumulated Stressor Value')
    ax1.set_xlabel('Component')
    ax1.tick_params(axis='x', rotation=45)
    ax1.grid(axis='y', alpha=0.3)
    
    # Add value labels on bars
    for bar in bars1:
        height = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2., height,
                f'{height:.2e}', ha='center', va='bottom', fontsize=8)
    
    # Plot 2: Stacked bar chart - Mechanisms per component
    ax2 = axes_flat[1]
    
    # Collect all mechanisms
    all_mechanisms = set()
    for mech_dict in accumulated_data.values():
        all_mechanisms.update(mech_dict.keys())
    all_mechanisms = sorted(all_mechanisms)
    
    # Prepare data for stacked bars
    mechanism_data = {mech: [] for mech in all_mechanisms}
    for comp in components:
        for mech in all_mechanisms:
            value = accumulated_data[comp].get(mech, 0)
            mechanism_data[mech].append(value)
    
    x_pos = np.arange(len(components))
    width = 0.6
    bottom = np.zeros(len(components))
    colors = plt.cm.Set3(np.linspace(0, 1, len(all_mechanisms)))
    
    for i, (mech, values) in enumerate(mechanism_data.items()):
        if any(v > 0 for v in values):  # Only plot if there's data
            ax2.bar(x_pos, values, width, label=mech, bottom=bottom, 
                   color=colors[i], alpha=0.8)
            bottom += values
    
    ax2.set_title('Accumulated Stressors by Mechanism (Stacked)', fontweight='bold')
    ax2.set_ylabel('Accumulated Stressor Value')
    ax2.set_xlabel('Component')
    ax2.set_xticks(x_pos)
    ax2.set_xticklabels(components, rotation=45)
    ax2.legend(bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=8)
    ax2.grid(axis='y', alpha=0.3)
    
    # Plot 3: Grouped bar chart - Mechanisms per component
    ax3 = axes_flat[2]
    
    x = np.arange(len(components))
    width_group = 0.8 / len(all_mechanisms) if all_mechanisms else 0.8
    
    for i, mech in enumerate(all_mechanisms):
        values = [accumulated_data[comp].get(mech, 0) for comp in components]
        if any(v > 0 for v in values):  # Only plot if there's data
            offset = (i - len(all_mechanisms)/2) * width_group + width_group/2
            ax3.bar(x + offset, values, width_group, label=mech, 
                   color=colors[i], alpha=0.8)
    
    ax3.set_title('Accumulated Stressors by Mechanism (Grouped)', fontweight='bold')
    ax3.set_ylabel('Accumulated Stressor Value')
    ax3.set_xlabel('Component')
    ax3.set_xticks(x)
    ax3.set_xticklabels(components, rotation=45)
    ax3.legend(bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=8)
    ax3.grid(axis='y', alpha=0.3)
    ax3.set_yscale('log')  # Use log scale for better visualization
    
    # Plot 4: Heatmap
    ax4 = axes_flat[3]
    
    # Create matrix for heatmap
    matrix = []
    mech_labels = []
    for comp in components:
        row = []
        for mech in all_mechanisms:
            value = accumulated_data[comp].get(mech, 0)
            row.append(value)
        matrix.append(row)
        if not mech_labels:
            mech_labels = all_mechanisms
    
    matrix = np.array(matrix)
    
    # Only create heatmap if there's data
    if matrix.size > 0 and matrix.max() > 0:
        im = ax4.imshow(matrix, aspect='auto', cmap='YlOrRd', 
                       interpolation='nearest')
        ax4.set_title('Accumulated Stressors Heatmap', fontweight='bold')
        ax4.set_xticks(np.arange(len(mech_labels)))
        ax4.set_yticks(np.arange(len(components)))
        ax4.set_xticklabels(mech_labels, rotation=45, ha='right')
        ax4.set_yticklabels(components)
        ax4.set_xlabel('Failure Mechanism')
        ax4.set_ylabel('Component')
        
        # Add text annotations
        for i in range(len(components)):
            for j in range(len(mech_labels)):
                value = matrix[i, j]
                if value > 0:
                    text = ax4.text(j, i, f'{value:.1e}',
                                   ha="center", va="center", 
                                   color="black", fontsize=7)
        
        plt.colorbar(im, ax=ax4, label='Accumulated Stressor Value')
    
    plt.tight_layout()
    
    if output_file:
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Plot saved to {output_file}")
    else:
        plt.show()

def print_summary(accumulated_data):
    """Print a summary of accumulated stressors."""
    print("\n" + "="*70)
    print("ACCUMULATED STRESSORS SUMMARY")
    print("="*70)
    
    for component in sorted(accumulated_data.keys()):
        print(f"\n{component.upper()}:")
        print("-" * 50)
        total = 0
        for mechanism, value in sorted(accumulated_data[component].items()):
            if value > 0:
                print(f"  {mechanism:30s}: {value:15.6e}")
                total += value
        print(f"  {'TOTAL':30s}: {total:15.6e}")
    
    print("\n" + "="*70)

def main():
    parser = argparse.ArgumentParser(
        description='Plot accumulated stressors for each failure mechanism on each component'
    )
    parser.add_argument(
        '--results-dir',
        type=str,
        default=None,
        help='Directory containing stressor CSV files (default: ./results)'
    )
    parser.add_argument(
        '--pattern',
        type=str,
        default='stressor_round_*.csv',
        help='File pattern to match (default: stressor_round_*.csv)'
    )
    parser.add_argument(
        '--output',
        type=str,
        default=None,
        help='Output file path for the plot (default: show interactively)'
    )
    parser.add_argument(
        '--summary',
        action='store_true',
        help='Print summary statistics to console'
    )
    
    args = parser.parse_args()
    
    # Load data
    print("Loading stressor files...")
    data_dict = load_stressor_files(args.pattern, args.results_dir)
    print(f"Loaded {len(data_dict)} file(s)")
    
    # Accumulate stressors
    print("Accumulating stressors...")
    accumulated = accumulate_stressors(data_dict)
    
    # Print summary if requested
    if args.summary:
        print_summary(accumulated)
    
    # Create plots
    print("Creating plots...")
    plot_accumulated_stressors(accumulated, output_file=args.output)
    
    print("Done!")

if __name__ == '__main__':
    main()

