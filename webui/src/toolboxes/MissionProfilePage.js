import React, { useState } from "react";

const ATTRIBUTE_OPTIONS = [
  "surface_albedo",
  "relative_humidity",
  "air_temperature",
  "dew_point",
  "surface_pressure",
  "wind_direction",
  "wind_speed",
  "ghi",
  "dni",
  "dhi",
  "solar_zenith_angle"
];
const DEFAULT_INTERVAL = "5";
const DEFAULT_LEAP_DAY = "true";
const DEFAULT_TO_UTC = "false";
const DEFAULT_YEAR = "2021";
const API_KEY = process.env.REACT_APP_NSRDB_API_KEY || "";
const EMAIL = "tracepv2022@outlook.com";

const MissionProfilePage = () => {
  // Option 1 states
  const [attributes, setAttributes] = useState(["surface_albedo", "relative_humidity"]);
  const [interval, setInterval] = useState(DEFAULT_INTERVAL);
  const [includeLeapDay, setIncludeLeapDay] = useState(DEFAULT_LEAP_DAY);
  const [toUTC, setToUTC] = useState(DEFAULT_TO_UTC);
  const [names, setNames] = useState(DEFAULT_YEAR);
  const [latitude, setLatitude] = useState("");
  const [longitude, setLongitude] = useState("");
  const [output, setOutput] = useState("");

  // For API call
  const [apiResponse, setApiResponse] = useState(null);
  const [apiLoading, setApiLoading] = useState(false);
  const [apiError, setApiError] = useState("");

  // Option 2 states
  const [fileName, setFileName] = useState("");
  const [fileContent, setFileContent] = useState("");

  // Handlers
  const handleAttributeChange = (attr) => {
    setAttributes(prev =>
      prev.includes(attr)
        ? prev.filter(a => a !== attr)
        : [...prev, attr]
    );
  };

  const handleOption1Submit = (e) => {
    e.preventDefault();
    if (!latitude || !longitude) {
      alert("Please provide both latitude and longitude.");
      return;
    }
    const config = {
      API_KEY,
      API_VERSION: "2.0",
      EMAIL,
      BASE_URL: "https://developer.nrel.gov/api/nsrdb/v2/solar/nsrdb-GOES-conus-v4-0-0-download.json?",
      ATTRIBUTES: attributes.join(","),
      INTERVAL: interval,
      INCLUDE_LEAP_DAY: includeLeapDay,
      TO_UTC: toUTC,
      NAMES: names,
      LATITUDE: latitude,
      LONGITUDE: longitude
    };
    setOutput(JSON.stringify(config, null, 2));
  };

  const handleDownloadConfig = () => {
    const blob = new Blob([output], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "nsrdb_config.json";
    a.click();
    URL.revokeObjectURL(url);
  };

  const handleFileChange = (e) => {
    const file = e.target.files[0];
    if (!file) return;
    setFileName(file.name);
    const reader = new FileReader();
    reader.onload = (event) => setFileContent(event.target.result);
    reader.readAsText(file);
  };

  // API Request Function
  const sendApiRequest = async () => {
    setApiError("");
    setApiResponse(null);
    setApiLoading(true);

    const input_data = {
      attributes: attributes.join(","),
      interval,
      include_leap_day: includeLeapDay,
      to_utc: toUTC,
      api_key: API_KEY,
      email: EMAIL,
      names: [names],
      location_ids: `${latitude},${longitude}`
    };

    try {
      const response = await fetch(
        "https://developer.nrel.gov/api/nsrdb/v2/solar/nsrdb-GOES-conus-v4-0-0-download.json?",
        {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "x-api-key": API_KEY
          },
          body: JSON.stringify(input_data)
        }
      );
      if (!response.ok) {
        const text = await response.text();
        throw new Error(`API error: ${response.status} ${response.statusText}\n${text}`);
      }
      const data = await response.json();
      if (data.errors && data.errors.length > 0) {
        setApiError(data.errors.join("\n"));
      } else {
        setApiResponse(data.outputs);
      }
    } catch (err) {
      setApiError(String(err));
    } finally {
      setApiLoading(false);
    }
  };

  return (
    <div style={{
      minHeight: "100vh",
      background: "linear-gradient(135deg,#e0ecff 0%,#f9fafc 100%)",
      display: "flex",
      alignItems: "flex-start",
      justifyContent: "center",
      flexDirection: "row",
      gap: 40,
      padding: "64px 0"
    }}>
      {/* Option 1 */}
      <div style={{
        background: "#fff",
        borderRadius: 18,
        boxShadow: "0 8px 32px 0 rgba(40,90,200,0.09)",
        padding: 32,
        minWidth: 320,
        maxWidth: 440,
        width: "100%"
      }}>
        <h2 style={{ color: "#2e466c", marginBottom: 18 }}>Option 1: Enter NSRDB Parameters</h2>
        <form style={{ display: "flex", flexDirection: "column", gap: 13 }} onSubmit={handleOption1Submit}>
          <label>
            Year (names):
            <input type="text" value={names} onChange={e => setNames(e.target.value)} style={inputStyle} />
          </label>
          <label>
            Interval (minutes):
            <input type="number" value={interval} onChange={e => setInterval(e.target.value)} style={inputStyle} />
          </label>
          <label>
            Include Leap Day:
            <select value={includeLeapDay} onChange={e => setIncludeLeapDay(e.target.value)} style={inputStyle}>
              <option value="true">true</option>
              <option value="false">false</option>
            </select>
          </label>
          <label>
            To UTC:
            <select value={toUTC} onChange={e => setToUTC(e.target.value)} style={inputStyle}>
              <option value="false">false</option>
              <option value="true">true</option>
            </select>
          </label>
          <label>
            <div style={{ marginBottom: 7 }}>Attributes (select one or more):</div>
            <div style={{ display: "flex", flexWrap: "wrap", gap: "10px 18px" }}>
              {ATTRIBUTE_OPTIONS.map(attr => (
                <label key={attr} style={{ fontWeight: 400, fontSize: 15 }}>
                  <input
                    type="checkbox"
                    value={attr}
                    checked={attributes.includes(attr)}
                    onChange={() => handleAttributeChange(attr)}
                    style={{ marginRight: 5 }}
                  />
                  {attr}
                </label>
              ))}
            </div>
          </label>
          <div style={{ display: "flex", gap: 12 }}>
            <div style={{ flex: 1 }}>
              <label>
                Latitude:
                <input type="number" value={latitude} onChange={e => setLatitude(e.target.value)} style={inputStyle} step="any" />
              </label>
            </div>
            <div style={{ flex: 1 }}>
              <label>
                Longitude:
                <input type="number" value={longitude} onChange={e => setLongitude(e.target.value)} style={inputStyle} step="any" />
              </label>
            </div>
          </div>
          <button type="submit" style={buttonStyle}>Generate Config</button>
        </form>
        {output &&
          <div style={{
            background: "#f6f8fa",
            borderRadius: 8,
            marginTop: 18,
            padding: 14,
            fontSize: 14,
            wordBreak: "break-all"
          }}>
            <b>Python Config:</b>
            <pre style={{ margin: 0 }}>{output}</pre>
            <button
              style={{ ...buttonStyle, marginTop: 8, padding: "8px 0", fontSize: 15 }}
              onClick={handleDownloadConfig}
            >
              Download JSON
            </button>

            {/* New: Send API Request */}
            <button
              type="button"
              onClick={sendApiRequest}
              style={{ ...buttonStyle, marginTop: 8, background: "linear-gradient(90deg,#ffb947 60%, #f7c85d 100%)" }}
              disabled={apiLoading}
            >
              {apiLoading ? "Requesting..." : "Send API Request"}
            </button>

            {apiError && (
              <div style={{ color: "red", marginTop: 10 }}>
                <b>API Error:</b><br />{apiError}
              </div>
            )}

            {apiResponse && (
              <div style={{
                marginTop: 16, background: "#f6f8fa", padding: 10, borderRadius: 7
              }}>
                <b>{apiResponse.message}</b>
                <div style={{ marginTop: 6 }}>
                  {apiResponse.downloadUrl && (
                    <a href={apiResponse.downloadUrl} target="_blank" rel="noopener noreferrer" style={{ color: "#1155cc" }}>
                      Download Data
                    </a>
                  )}
                </div>
                <pre style={{ fontSize: 13, marginTop: 12 }}>
                  {JSON.stringify(apiResponse, null, 2)}
                </pre>
              </div>
            )}
          </div>
        }
      </div>
      {/* Option 2 */}
      <div style={{
        background: "#fff",
        borderRadius: 18,
        boxShadow: "0 8px 32px 0 rgba(40,90,200,0.09)",
        padding: 32,
        minWidth: 320,
        maxWidth: 430,
        width: "100%",
        alignSelf: "flex-start"
      }}>
        <h2 style={{ color: "#2e466c", marginBottom: 18 }}>Option 2: Upload Profile File</h2>
        <input
          type="file"
          accept=".csv,.txt"
          onChange={handleFileChange}
          style={{ marginBottom: 18 }}
        />
        {fileName && (
          <div>
            <div style={{ fontWeight: 600, color: "#33506a" }}>File: {fileName}</div>
            <pre style={{
              background: "#f6f8fa",
              padding: 10,
              borderRadius: 6,
              maxHeight: 200,
              overflowY: "auto",
              marginTop: 7,
              fontSize: 14
            }}>
              {fileContent.slice(0, 1000) || "File loaded."}
              {fileContent.length > 1000 && "\n... (truncated)"}
            </pre>
          </div>
        )}
      </div>
    </div>
  );
};

const inputStyle = {
  width: "100%",
  padding: "10px",
  borderRadius: 7,
  border: "1px solid #c4d0eb",
  fontSize: 15,
  marginTop: 3,
  marginBottom: 2,
  boxSizing: "border-box",
  background: "#f6f8fa"
};

const buttonStyle = {
  width: "100%",
  padding: "12px 0",
  background: "linear-gradient(90deg,#4882ff 60%, #76b5ff 100%)",
  color: "#fff",
  fontSize: 17,
  fontWeight: 700,
  border: "none",
  borderRadius: 8,
  marginTop: 8,
  cursor: "pointer"
};

export default MissionProfilePage;
