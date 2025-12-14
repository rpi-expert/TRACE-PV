#!/usr/bin/env python3
"""
NSRDB Data Download Script
Uses virtual environment at ./venv/bin/python3 if available
"""

import pandas as pd
import urllib.parse
import urllib.request
import json
import requests
import time

# Based on: https://developer.nrel.gov/docs/solar/nsrdb/nsrdb-GOES-conus-v4-0-0-download/
BASE_URL_CSV = "https://developer.nrel.gov/api/nsrdb/v2/solar/nsrdb-GOES-conus-v4-0-0-download.csv"
BASE_URL_JSON = "https://developer.nrel.gov/api/nsrdb/v2/solar/nsrdb-GOES-conus-v4-0-0-download.json"

API_KEY = "HQD026KeiVhDH8aXrrkCJ9iGFZCj2AQyNvM1cqnx"
EMAIL = "wangl27@rpi.edu"  # Required for API tracking

# You can provide either location_ids or coordinates
# Format: location_id (string) or [latitude, longitude] (list)
# Coordinates format: [latitude, longitude]
POINTS = [
    [33.965247, -81.074012],  # lat, lon for location_id 2494888
    # You can add more points as needed:
    # [lat1, lon1],
    # [lat2, lon2],
    # Or use location_id (will try to lookup coordinates):
    # '2494888'
]

def get_coordinates_from_location_id(location_id):
    """
    Get latitude and longitude from a location_id using the point-attributes endpoint.
    Returns (lat, lon) tuple or None if failed.
    """
    try:
        check_url = "https://developer.nrel.gov/api/nsrdb/v2/solar/point-attributes.json"
        params = {
            'api_key': API_KEY,
            'email': EMAIL,
            'location_ids': location_id
        }
        
        response = requests.get(check_url, params=params, timeout=30)
        if response.status_code == 200:
            data = response.json()
            if 'outputs' in data and len(data['outputs']) > 0:
                output = data['outputs'][0]
                lat = output.get('latitude')
                lon = output.get('longitude')
                if lat is not None and lon is not None:
                    return (lat, lon)
                else:
                    return None
            elif 'errors' in data:
                return None
        else:
            return None
    except Exception as e:
        return None

def create_wkt_from_coordinates(lat, lon):
    """
    Create WKT (Well-Known Text) POINT format from coordinates.
    Format: POINT(lon lat) - note: longitude comes first!
    """
    return f"POINT({lon} {lat})"

def download_csv_direct(wkt, year, attributes='air_temperature', interval='5', leap_day=True):
    """
    Method 1: Direct CSV download (only works for single POINT, single YEAR)
    Based on API docs: CSV format is restricted to single point, single year
    """
    try:
        params = {
            'api_key': API_KEY,
            'wkt': wkt,
            'names': year,  # Single year as string
            'attributes': attributes,
            'interval': interval,
            'leap_day': 'true' if leap_day else 'false',
            'email': EMAIL
        }
        
        query_url = BASE_URL_CSV + '?' + urllib.parse.urlencode(params)
        
        df = pd.read_csv(query_url)
        
        # Check for errors in response
        if len(df.columns) == 1 and ('error' in str(df.columns[0]).lower() or 'errors' in str(df.columns[0]).lower()):
            return None
        
        if len(df) == 0:
            return None
        
        return df
        
    except urllib.error.HTTPError as e:
        return None
    except Exception as e:
        return None

def download_via_json_endpoint(wkt, year, attributes='air_temperature', interval='5', leap_day=True):
    """
    Method 2: Use JSON endpoint (for getting download URL or direct data)
    """
    try:
        # For single point, single year, we can try the JSON endpoint
        # But note: JSON endpoint typically returns download URL, not direct data
        params = {
            'api_key': API_KEY,
            'wkt': wkt,
            'names': year,
            'attributes': attributes,
            'interval': interval,
            'leap_day': 'true' if leap_day else 'false',
            'email': EMAIL
        }
        
        query_url = BASE_URL_JSON + '?' + urllib.parse.urlencode(params)
        
        with urllib.request.urlopen(query_url, timeout=60) as response:
            data = json.loads(response.read().decode())
            
            if 'errors' in data and len(data['errors']) > 0:
                return None
            
            # Check if we got a download URL
            if 'outputs' in data and 'downloadUrl' in data['outputs']:
                download_url = data['outputs']['downloadUrl']
                
                # Wait for file to be ready
                time.sleep(10)
                
                # Try to download
                try:
                    df = pd.read_csv(download_url)
                    return df
                except Exception as download_err:
                    return None
            else:
                return None
                
    except Exception as e:
        return None

def download_via_post_request(wkt, year, attributes='air_temperature', interval='5', leap_day=True):
    """
    Method 3: POST request to download endpoint
    Based on API docs example
    """
    try:
        url = f"{BASE_URL_JSON}?api_key={API_KEY}"
        
        # Prepare payload as form data (per API docs example)
        payload = {
            'wkt': wkt,
            'names': year,
            'attributes': attributes,
            'interval': interval,
            'leap_day': 'true' if leap_day else 'false',
            'email': EMAIL
        }
        
        headers = {
            'content-type': 'application/x-www-form-urlencoded',
            'cache-control': 'no-cache'
        }
        
        response = requests.post(url, data=payload, headers=headers, timeout=60)
        
        if response.status_code != 200:
            return None
        
        data = response.json()
        
        if 'errors' in data and len(data['errors']) > 0:
            return None
        
        if 'outputs' in data and 'downloadUrl' in data['outputs']:
            download_url = data['outputs']['downloadUrl']
            
            # Wait for file to be ready
            time.sleep(10)
            
            # Try to download
            try:
                df = pd.read_csv(download_url)
                return df
            except Exception as download_err:
                return None
        else:
            return None
            
    except Exception as e:
        return None

def convert_to_mission_profile(input_filename):
    """
    Convert NSRDB downloaded file to mission_profile.csv format.
    Output: time (datetime), GHI, ambient_temp, rh
    
    Returns:
        True if successful, False otherwise
    """
    try:
        # Read the CSV file, skipping the first 2 metadata rows
        # The data starts at row 3 (index 2) with headers: Year,Month,Day,Hour,Minute,GHI,Temperature,Relative Humidity
        df = pd.read_csv(input_filename, skiprows=2)
        
        # Create datetime from Year, Month, Day, Hour, Minute columns
        df['time'] = pd.to_datetime(df[['Year', 'Month', 'Day', 'Hour', 'Minute']])
        
        # Select and rename columns
        mission_df = pd.DataFrame({
            'time': df['time'],
            'GHI': df['GHI'],
            'ambient_temp': df['Temperature'],  # Temperature is ambient temperature
            'rh': df['Relative Humidity']  # Relative Humidity
        })
        
        # Save as mission_profile.csv
        output_filename = 'simulator_inputs/mission_profile/environmental_condition/mission_profile.csv'
        mission_df.to_csv(output_filename, index=False)

        print(f"Mission profile saved to: {output_filename}")
        
        return True
        
    except FileNotFoundError:
        return False
    except KeyError as e:
        return False
    except Exception as e:
        return False

def main():
    # Request only: GHI, ambient temperature, relative humidity
    # Available attributes: ghi, air_temperature, relative_humidity
    attributes = 'ghi,air_temperature,relative_humidity'
    interval = '5'
    leap_day = True
    
    for year in ['2024']:
        print(f"Year: {year}")
        
        for idx, point in enumerate(POINTS):
            # Check if point is location_id (string/int) or coordinates (list/tuple)
            if isinstance(point, (list, tuple)) and len(point) == 2:
                # Already have coordinates
                lat, lon = float(point[0]), float(point[1])
                wkt = create_wkt_from_coordinates(lat, lon)
                print(f"Location: lat={lat}, lon={lon}")
            else:
                # Need to get coordinates from location_id
                coords = get_coordinates_from_location_id(point)
                if not coords:
                    continue
                lat, lon = coords
                wkt = create_wkt_from_coordinates(lat, lon)
                print(f"Location: lat={lat}, lon={lon}")
            
            # Try Method 1: Direct CSV (fastest for single point, single year)
            df = download_csv_direct(wkt, year, attributes, interval, leap_day)
            
            if df is None:
                # Try Method 2: JSON endpoint
                df = download_via_json_endpoint(wkt, year, attributes, interval, leap_day)
            
            if df is None:
                # Try Method 3: POST request
                df = download_via_post_request(wkt, year, attributes, interval, leap_day)
            
            if df is not None:
                # Save the original downloaded file temporarily
                original_filename = f'nsrdb_{point}_{year}.csv'
                df.to_csv(original_filename, index=False)
                
                # Convert to mission_profile.csv format
                if convert_to_mission_profile(original_filename):
                    # Delete the original file after successful conversion
                    import os
                    try:
                        os.remove(original_filename)
                    except Exception as e:
                        pass

if __name__ == "__main__":
    main()
