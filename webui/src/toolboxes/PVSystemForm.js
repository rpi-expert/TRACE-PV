// src/PVSystemForm.js
import React, { useState, useEffect } from 'react';
import optionsData from '../PV panel JSON files/topologies_options.json';

function saveFormDataAsJson(formData, filename = "pv_system_config.json") {
  const dataStr = JSON.stringify(formData, null, 2);
  const blob = new Blob([dataStr], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

const PVSystemForm = () => {
  const [formData, setFormData] = useState({
    topologyOption: "CS6U-330P",
    parallelStrings: '66',
    seriesModules: '5'
  });
  const [panelData, setPanelData] = useState(null);
  const options = optionsData.top_options;

  const fetchPanelData = async (topologyOption) => {
    try {
      const newvalue = topologyOption.replace(/\//g, '-');
      const panelModule = await import(`../PV panel JSON files/${newvalue}.json`);
      setPanelData(panelModule.default);
    } catch (error) {
      console.error('Error fetching PV panel data:', error);
      setPanelData(null);
    }
  };

  useEffect(() => {
    fetchPanelData(formData.topologyOption);
    // Only on mount and when topologyOption changes
    // eslint-disable-next-line
  }, [formData.topologyOption]);

  const handleChange = async (e) => {
    const { name, value } = e.target;
    setFormData({ ...formData, [name]: value });

    if (name === 'topologyOption') {
      fetchPanelData(value);
    }
  };

  const handleSubmit = (e) => {
    e.preventDefault();
    // Replace with your submit logic
    saveFormDataAsJson(formData);
    alert('Your configuration was saved as a JSON file!');
    alert('Form Submitted!\n' + JSON.stringify(formData, null, 2));
    console.log('Selected PV Panel Data:', panelData);
  };

  return (
    <div
      style={{
        minHeight: '100vh',
        width: '100vw',
        background: 'linear-gradient(135deg, #e0ecff 0%, #f9fafc 100%)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
      }}
    >
      <form
        onSubmit={handleSubmit}
        style={{
          background: '#fff',
          borderRadius: 22,
          boxShadow: '0 8px 32px 0 rgba(40,90,200,0.09)',
          padding: '42px 32px',
          minWidth: 320,
          maxWidth: 420,
          width: '100%',
        }}
      >
        <h2 style={{ fontWeight: 700, fontSize: 28, color: '#2e466c', marginBottom: 32 }}>
          PV System Setup
        </h2>
        <div style={{ marginBottom: 28 }}>
          <label
            style={{ display: 'block', fontWeight: 600, fontSize: 18, marginBottom: 8 }}
          >
            Topology Option
          </label>
          <select
            name="topologyOption"
            value={formData.topologyOption}
            onChange={handleChange}
            style={inputStyle}
          >
            {options.map(option => (
              <option key={option} value={option}>{option}</option>
            ))}
          </select>
        </div>
        <div style={{ marginBottom: 24 }}>
          <label
            style={{ display: 'block', fontWeight: 600, fontSize: 18, marginBottom: 8 }}
          >
            # of Parallel Strings
          </label>
          <input
            type="number"
            name="parallelStrings"
            value={formData.parallelStrings}
            onChange={handleChange}
            style={inputStyle}
            min="1"
            max="100"
          />
        </div>
        <div style={{ marginBottom: 34 }}>
          <label
            style={{ display: 'block', fontWeight: 600, fontSize: 18, marginBottom: 8 }}
          >
            # of Series Connected Modules per String
          </label>
          <input
            type="number"
            name="seriesModules"
            value={formData.seriesModules}
            onChange={handleChange}
            style={inputStyle}
            min="1"
            max="100"
          />
        </div>
        <button
          type="submit"
          style={{
            width: '100%',
            padding: '18px 0',
            background: 'linear-gradient(90deg,#4882ff 60%, #76b5ff 100%)',
            color: '#fff',
            fontSize: 20,
            fontWeight: 700,
            border: 'none',
            borderRadius: 12,
            boxShadow: '0 4px 12px 0 rgba(80,110,220,0.14)',
            cursor: 'pointer',
            letterSpacing: 0.2,
            transition: 'background 0.2s, transform 0.13s',
          }}
          onMouseOver={e => {
            e.currentTarget.style.background = 'linear-gradient(90deg, #356fe7 60%, #64a3f8 100%)';
            e.currentTarget.style.transform = 'translateY(-2px) scale(1.03)';
          }}
          onMouseOut={e => {
            e.currentTarget.style.background = 'linear-gradient(90deg,#4882ff 60%, #76b5ff 100%)';
            e.currentTarget.style.transform = 'none';
          }}
        >
        Submit
        </button>
      </form>
    </div>
  );
};

const inputStyle = {
  width: '100%',
  padding: '13px',
  borderRadius: 9,
  border: '1px solid #c4d0eb',
  fontSize: 16,
  marginTop: 3,
  marginBottom: 2,
  boxSizing: 'border-box',
  background: '#f6f8fa'
};

export default PVSystemForm;