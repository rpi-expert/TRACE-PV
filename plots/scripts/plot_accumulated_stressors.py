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
import sys
import re
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
        # Default to stressors directory (also check results as fallback)
        stressors_dir = Path(__file__).parent / "results"
        results_dir_default = Path(__file__).parent / "plots"
        if stressors_dir.exists():
            results_dir = stressors_dir
        else:
            results_dir = results_dir_default
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

def parse_filename(filename):
    """
    Parse filename to extract round and iteration numbers.
    
    Expected format: stressor_round_X_iteration_Y.csv
    
    Returns:
        tuple: (round_num, iteration_num) or (None, None) if not parseable
    """
    filename_str = str(filename) if isinstance(filename, Path) else filename
    match = re.search(r'round_(\d+).*iteration_(\d+)', filename_str, re.IGNORECASE)
    if match:
        return (int(match.group(1)), int(match.group(2)))
    return (None, None)

def combine_data(data_dict):
    """
    Combine all dataframes from multiple files into a single dataframe.
    Tracks iteration boundaries for marking in plots.
    
    Args:
        data_dict: Dictionary mapping file paths to DataFrames
    
    Returns:
        tuple: (combined_df, iteration_boundaries, round_boundaries)
        - combined_df: Combined DataFrame sorted by case_index with iteration info
        - iteration_boundaries: List of (event_id, round, iteration) tuples marking iteration ends
        - round_boundaries: List of (event_id, round, iteration) tuples marking round ends
    """
    if not data_dict:
        return pd.DataFrame(), [], []
    
    # Combine all dataframes with iteration and round tracking
    dfs = []
    iteration_boundaries = []
    round_boundaries = []
    current_event_id = 0
    
    # Sort files by iteration first, then round
    # Sequence: iteration 1 (rounds 1-6) -> iteration 2 (rounds 1-6) -> ...
    sorted_files = sorted(data_dict.items(), key=lambda x: (
        parse_filename(x[0])[1] or 0,  # iteration (primary sort)
        parse_filename(x[0])[0] or 0  # round (secondary sort)
    ))
    
    current_iteration = None
    current_round = None
    last_event_id_in_iteration = None
    
    for file_path, df in sorted_files:
        round_num, iter_num = parse_filename(file_path)
        
        # Add iteration info to dataframe
        df_copy = df.copy()
        if 'case_index' in df_copy.columns:
            # Adjust case_index to be continuous across iterations
            df_copy['original_case_index'] = df_copy['case_index']
            df_copy['case_index'] = df_copy['case_index'] + current_event_id
            df_copy['round'] = round_num if round_num is not None else -1
            df_copy['iteration'] = iter_num if iter_num is not None else -1
            
            if len(df_copy) > 0:
                max_event_id = df_copy['case_index'].max()
                
                # Mark round boundary (end of each round)
                if round_num is not None:
                    round_boundaries.append((max_event_id, round_num, iter_num))
                
                # Check if we've moved to a new iteration
                if current_iteration is not None and iter_num != current_iteration:
                    # Mark the end of the previous iteration
                    if last_event_id_in_iteration is not None:
                        iteration_boundaries.append((last_event_id_in_iteration, None, current_iteration))
                
                # Update tracking
                current_iteration = iter_num
                current_round = round_num
                last_event_id_in_iteration = max_event_id
                current_event_id = max_event_id + 1
        
        dfs.append(df_copy)
    
    # Mark the end of the last iteration
    if last_event_id_in_iteration is not None and current_iteration is not None:
        iteration_boundaries.append((last_event_id_in_iteration, None, current_iteration))
    
    combined_df = pd.concat(dfs, ignore_index=True)
    
    # Sort by case_index to ensure proper time series order
    if 'case_index' in combined_df.columns:
        combined_df = combined_df.sort_values('case_index').reset_index(drop=True)
    
    return combined_df, iteration_boundaries, round_boundaries

def get_component_mechanism_pairs(df):
    """
    Extract all component-mechanism pairs from the dataframe.
    
    Args:
        df: DataFrame with stressor columns
    
    Returns:
        List of tuples: [(component, mechanism, column_name), ...]
    """
    pairs = []
    # Exclude tracking columns: case_index, original_case_index, round, iteration
    excluded_cols = {'case_index', 'original_case_index', 'round', 'iteration'}
    stressor_cols = [col for col in df.columns if col not in excluded_cols]
    
    for col in stressor_cols:
        component, mechanism = parse_stressor_column(col)
        if mechanism:
            pairs.append((component, mechanism, col))
        else:
            # For components without explicit mechanism, use component name as mechanism
            pairs.append((component, component, col))
    
    return sorted(set(pairs))  # Remove duplicates and sort

def plot_combined_stressors(df, iteration_boundaries=None, round_boundaries=None, output_file=None, figsize=(20, 10)):
    """
    Create a standalone combined plot showing all stressors together with iteration markers.
    
    Args:
        df: Combined DataFrame with case_index and stressor columns
        iteration_boundaries: List of (event_id, round, iteration) tuples marking iteration ends
        output_file: Path to save the plot (optional)
        figsize: Figure size tuple
    """
    if df.empty or 'case_index' not in df.columns:
        print("Error: DataFrame is empty or missing 'case_index' column")
        return
    
    # Get all component-mechanism pairs
    pairs = get_component_mechanism_pairs(df)
    
    if not pairs:
        print("Error: No stressor columns found")
        return
    
    # Create standalone figure
    fig, ax = plt.subplots(figsize=figsize)
    fig.suptitle('Accumulated Stressors Over Time (All Combined)', 
                 fontsize=16, fontweight='bold')
    
    # Get color map for different stressors
    colors = plt.cm.tab20(np.linspace(0, 1, len(pairs)))
    
    # Plot all stressors in the combined plot
    for idx, (component, mechanism, col_name) in enumerate(pairs):
        if col_name in df.columns:
            cumulative = df[col_name].cumsum()
            event_ids = df['case_index']
            label = f'{component}-{mechanism}'
            
            # Only plot if there's data
            if cumulative.max() > 0 or cumulative.min() < 0:
                ax.plot(event_ids, cumulative, linewidth=2, 
                       label=label, color=colors[idx], alpha=0.8)
    
    # Get y-axis limits for label positioning
    y_min, y_max = ax.get_ylim()
    y_range = y_max - y_min
    
    # Add round boundary markers (blue, thinner lines)
    if round_boundaries:
        for event_id, round_num, iter_num in round_boundaries:
            ax.axvline(x=event_id, color='blue', linestyle=':', linewidth=0.8, alpha=0.4)
            # Add text label for round (positioned lower)
            if round_num is not None and iter_num is not None:
                ax.text(event_id, y_min + y_range * 0.05, 
                       f'R{round_num}', 
                       rotation=90, verticalalignment='bottom', 
                       fontsize=7, alpha=0.6, color='blue')
    
    # Add iteration boundary markers (red, thicker lines) - labeled as "Year"
    # Stagger vertical positions to avoid overlap when boundaries are close
    if iteration_boundaries:
        # Get x-axis range to determine if boundaries are close
        x_min, x_max = ax.get_xlim()
        x_range = x_max - x_min
        min_spacing = x_range * 0.05  # Consider boundaries "close" if within 5% of x-axis range
        
        boundary_positions = [event_id for event_id, _, _ in iteration_boundaries]
        
        for idx, (event_id, round_num, iter_num) in enumerate(iteration_boundaries):
            ax.axvline(x=event_id, color='red', linestyle='--', linewidth=1.5, alpha=0.7)
            # Add text label for year (horizontal, positioned higher, bigger font)
            if iter_num is not None:
                # Check if this boundary is close to the previous one
                vertical_offset = 0
                if idx > 0:
                    prev_event_id = boundary_positions[idx - 1]
                    spacing = event_id - prev_event_id
                    if spacing < min_spacing:
                        # If close to previous, offset vertically
                        vertical_offset = y_range * 0.04  # Offset by 4% of y-range
                
                y_pos = y_max - y_range * 0.05 + vertical_offset
                
                ax.text(event_id, y_pos, 
                       f'Year {iter_num}', 
                       rotation=0, horizontalalignment='center', verticalalignment='bottom', 
                       fontsize=14, alpha=0.8, color='red', fontweight='bold')
    
    ax.set_xlabel('Event ID (Time)', fontsize=12)
    ax.set_ylabel('Cumulative Stressor', fontsize=12)
    ax.grid(True, alpha=0.3)
    ax.ticklabel_format(style='scientific', axis='y', scilimits=(0, 0))
    
    # Add legend
    ax.legend(bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=9, 
              ncol=1, framealpha=0.9)
    
    plt.tight_layout()
    
    if output_file:
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Combined plot saved to {output_file}")
    else:
        plt.show()

def plot_accumulated_stressors(df, output_file=None, figsize=None):
    """
    Create visualizations of accumulated stressors over time (event_id).
    Each component-mechanism combination gets its own subplot showing cumulative sum.
    
    Args:
        df: Combined DataFrame with case_index and stressor columns
        output_file: Path to save the plot (optional)
        figsize: Figure size tuple (auto-calculated if None)
    """
    if df.empty or 'case_index' not in df.columns:
        print("Error: DataFrame is empty or missing 'case_index' column")
        return
    
    # Get all component-mechanism pairs
    pairs = get_component_mechanism_pairs(df)
    
    if not pairs:
        print("Error: No stressor columns found")
        return
    
    # Calculate number of subplots needed (only individual plots, combined is separate)
    n_plots = len(pairs)
    
    # Determine grid layout (try to make it roughly square)
    n_cols = int(np.ceil(np.sqrt(n_plots)))
    n_rows = int(np.ceil(n_plots / n_cols))
    
    # Auto-calculate figure size if not provided
    if figsize is None:
        figsize = (max(16, n_cols * 4), max(10, n_rows * 3))
    
    # Create figure with subplots
    fig, axes = plt.subplots(n_rows, n_cols, figsize=figsize)
    fig.suptitle('Accumulated Stressors Over Time', 
                 fontsize=16, fontweight='bold')
    
    # Flatten axes for easier iteration
    if n_plots == 1:
        axes = [axes]
    else:
        axes = axes.flatten() if hasattr(axes, 'flatten') else [axes]
    
    # Plot cumulative sum for each component-mechanism pair (individual subplots)
    for idx, (component, mechanism, col_name) in enumerate(pairs):
        ax = axes[idx]
        
        # Calculate cumulative sum over case_index (event_id)
        if col_name in df.columns:
            cumulative = df[col_name].cumsum()
            event_ids = df['case_index']
            
            # Plot the curve
            ax.plot(event_ids, cumulative, linewidth=2, label=f'{component}-{mechanism}')
            ax.set_title(f'{component}\n{mechanism}', fontweight='bold', fontsize=10)
            ax.set_xlabel('Event ID (Time)', fontsize=9)
            ax.set_ylabel('Cumulative Stressor', fontsize=9)
            ax.grid(True, alpha=0.3)
            
            # Use scientific notation for y-axis if values are very small/large
            if cumulative.max() > 0:
                ax.ticklabel_format(style='scientific', axis='y', scilimits=(0, 0))
        else:
            ax.text(0.5, 0.5, 'No data', ha='center', va='center', transform=ax.transAxes)
            ax.set_title(f'{component}\n{mechanism}', fontweight='bold', fontsize=10)
    
    # Hide unused subplots (if any)
    for idx in range(n_plots, len(axes)):
        axes[idx].axis('off')
    
    plt.tight_layout()
    
    if output_file:
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Plot saved to {output_file}")
    else:
        plt.show()

def print_summary(df):
    """Print a summary of accumulated stressors."""
    if df.empty:
        print("No data to summarize")
        return
    
    print("\n" + "="*70)
    print("ACCUMULATED STRESSORS SUMMARY")
    print("="*70)
    
    pairs = get_component_mechanism_pairs(df)
    
    for component, mechanism, col_name in pairs:
        if col_name in df.columns:
            total = df[col_name].sum()
            final_cumulative = df[col_name].cumsum().iloc[-1] if len(df) > 0 else 0
            print(f"{component:20s} | {mechanism:30s} | Total: {total:15.6e} | Final Cumulative: {final_cumulative:15.6e}")
    
    print("\n" + "="*70)

def main():
    parser = argparse.ArgumentParser(
        description='Plot accumulated stressors for each failure mechanism on each component'
    )
    parser.add_argument(
        '--results-dir',
        type=str,
        default=None,
        help='Directory containing stressor CSV files (default: ./stressors, fallback: ./results)'
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
    
    # Combine all dataframes with iteration and round tracking
    print("Combining data...")
    combined_df, iteration_boundaries, round_boundaries = combine_data(data_dict)
    
    if combined_df.empty:
        print("ERROR: No data to plot!")
        sys.exit(1)
    
    print(f"Total records: {len(combined_df)}")
    if 'case_index' in combined_df.columns:
        print(f"Event ID range: {combined_df['case_index'].min()} to {combined_df['case_index'].max()}")
    
    if round_boundaries:
        print(f"Round boundaries found: {len(round_boundaries)}")
    if iteration_boundaries:
        print(f"Year boundaries found: {len(iteration_boundaries)}")
        print(f"  (Each year contains rounds 1-6)")
        for event_id, round_num, iter_num in iteration_boundaries[:5]:  # Show first 5
            if iter_num is not None:
                print(f"  Year {iter_num}: ends at event_id {event_id}")
        if len(iteration_boundaries) > 5:
            print(f"  ... and {len(iteration_boundaries) - 5} more")
    
    # Print summary if requested
    if args.summary:
        print_summary(combined_df)
    
    # Create individual subplots
    print("Creating individual subplots...")
    individual_output = None
    combined_output = None
    if args.output:
        # If output specified, create separate files
        base_path = Path(args.output)
        individual_output = str(base_path.parent / f"{base_path.stem}_individual{base_path.suffix}")
        combined_output = str(base_path.parent / f"{base_path.stem}_combined{base_path.suffix}")
        
        # Print output file paths
        print(f"\nOutput files will be saved to:")
        print(f"  Individual subplots: {individual_output}")
        print(f"  Combined plot: {combined_output}")
        print(f"  Output directory: {base_path.parent.absolute()}")
    else:
        combined_output = None  # Will show interactively
        print("\nPlots will be displayed interactively (no files saved)")
    
    plot_accumulated_stressors(combined_df, output_file=individual_output)
    
    # Create standalone combined plot with iteration and round markers
    print("Creating combined plot with iteration and round markers...")
    plot_combined_stressors(combined_df, iteration_boundaries, round_boundaries, output_file=combined_output)
    
    if args.output:
        print(f"\n✓ All plots saved successfully!")
        print(f"  Individual subplots: {Path(individual_output).absolute()}")
        print(f"  Combined plot: {Path(combined_output).absolute()}")
    
    print("Done!")

if __name__ == '__main__':
    main()

