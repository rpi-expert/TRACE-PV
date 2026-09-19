// src/GridTransformerForm.js
import React, { useState } from 'react';

function saveFormDataAsJson(formData, filename = "grid_transformer_config.json") {
  const dataStr = JSON.stringify(formData, null, 2);
  const blob = new Blob([dataStr], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}


const connectionOptions = [
  { label: 'Y-grounded', value: 'Y-grounded' },
  { label: 'Y', value: 'Y' },
  { label: 'Delta', value: 'Delta' },
];

const GridTransformerForm = () => {
  const [form, setForm] = useState({
    gridVoltage: '',
    gridFrequency: '',
    gridPhase: '',
    gridPower: '',
    gridImpedance: '',
    gridOutputConnection: '',
    gridPrimaryConfig: '',
    gridSecondaryConfig: '',
    transformerPrimaryVoltage: '',
    transformerSecondaryVoltage: '',
    transformerConfig: '',
    transformerVoltageRatings: '',
    transformerPowerRatings: ''
  });

  const handleChange = (e) => {
    setForm({
      ...form,
      [e.target.name]: e.target.value
    });
  };

  const handleSubmit = (e) => {
    e.preventDefault();
    saveFormDataAsJson(form);
    alert('Your configuration was saved as a JSON file!');
    // TODO: handle submit logic
    alert('Submitted!\n' + JSON.stringify(form, null, 2));
  };

  return (
    <div
      style={{
        minHeight: '100vh',
        background: 'linear-gradient(135deg, #e0ecff 0%, #f9fafc 100%)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        width: '100vw'
      }}
    >
    <form
      onSubmit={handleSubmit}
      style={{
        background: '#fff',
        borderRadius: 22,
        boxShadow: '0 8px 32px 0 rgba(40,90,200,0.09)',
        padding: '44px 36px',
        minWidth: 340,
        maxWidth: 900,
        width: '100%',
      }}
    >
      <h2 style={{ fontWeight: 700, fontSize: 28, color: '#2e466c', marginBottom: 24, gridColumn: '1 / -1' }}>
        Grid and Transformer
      </h2>
      <div
        style={{
          display: 'grid',
          gridTemplateColumns: '1fr 1fr',
          gap: 30,
          marginBottom: 22,
        }}
      >
        {/* COLUMN 1: GRID */}
        <div>
          <h3 style={{ color: '#4b6697', marginTop: 8, marginBottom: 10, fontSize: 19, fontWeight: 600 }}>
            Grid
          </h3>
          <div style={{ marginBottom: 18 }}>
            <label>Grid Voltage</label>
            <input name="gridVoltage" value={form.gridVoltage} onChange={handleChange} type="text"
              placeholder="e.g. 480V" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Grid Frequency</label>
            <input name="gridFrequency" value={form.gridFrequency} onChange={handleChange} type="text"
              placeholder="e.g. 60Hz" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Grid Phase</label>
            <input name="gridPhase" value={form.gridPhase} onChange={handleChange} type="text"
              placeholder="e.g. 3-phase" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Grid Power</label>
            <input name="gridPower" value={form.gridPower} onChange={handleChange} type="text"
              placeholder="e.g. 1MW" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Grid Impedance</label>
            <input name="gridImpedance" value={form.gridImpedance} onChange={handleChange} type="text"
              placeholder="e.g. 0.1Ω" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Grid Output Connection</label>
            <input name="gridOutputConnection" value={form.gridOutputConnection} onChange={handleChange} type="text"
              placeholder="e.g. Wye" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Connection configuration on the primary side</label>
            <select name="gridPrimaryConfig" value={form.gridPrimaryConfig} onChange={handleChange} style={inputStyle}>
              <option value="">Select</option>
              {connectionOptions.map(opt => <option key={opt.value} value={opt.value}>{opt.label}</option>)}
            </select>
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Connection configuration on the secondary side</label>
            <select name="gridSecondaryConfig" value={form.gridSecondaryConfig} onChange={handleChange} style={inputStyle}>
              <option value="">Select</option>
              {connectionOptions.map(opt => <option key={opt.value} value={opt.value}>{opt.label}</option>)}
            </select>
          </div>
        </div>
        {/* COLUMN 2: TRANSFORMER */}
        <div>
          <h3 style={{ color: '#4b6697', marginTop: 8, marginBottom: 10, fontSize: 19, fontWeight: 600 }}>
            Transformer
          </h3>
          <div style={{ marginBottom: 18 }}>
            <label>Transformer voltage ratings on the primary side</label>
            <input name="transformerPrimaryVoltage" value={form.transformerPrimaryVoltage} onChange={handleChange} type="text"
              placeholder="e.g. 34.5kV" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Transformer voltage ratings on the secondary side</label>
            <input name="transformerSecondaryVoltage" value={form.transformerSecondaryVoltage} onChange={handleChange} type="text"
              placeholder="e.g. 480V" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Transformer Configuration</label>
            <select name="transformerConfig" value={form.transformerConfig} onChange={handleChange} style={inputStyle}>
              <option value="">Select</option>
              {connectionOptions.map(opt => <option key={opt.value} value={opt.value}>{opt.label}</option>)}
            </select>
          </div>
          <div style={{ marginBottom: 18 }}>
            <label>Transformer Voltage Ratings</label>
            <input name="transformerVoltageRatings" value={form.transformerVoltageRatings} onChange={handleChange} type="text"
              placeholder="e.g. 34.5kV/480V" style={inputStyle} />
          </div>
          <div style={{ marginBottom: 32 }}>
            <label>Transformer Power Ratings</label>
            <input name="transformerPowerRatings" value={form.transformerPowerRatings} onChange={handleChange} type="text"
              placeholder="e.g. 2MVA" style={inputStyle} />
          </div>
        </div>
      </div>
      <button type="submit"
        style={{
          width: '100%',
          padding: '15px 0',
          background: 'linear-gradient(90deg,#4882ff 60%, #76b5ff 100%)',
          color: '#fff',
          fontSize: 19,
          fontWeight: 700,
          border: 'none',
          borderRadius: 12,
          boxShadow: '0 4px 12px 0 rgba(80,110,220,0.14)',
          cursor: 'pointer',
          transition: 'background 0.18s, transform 0.12s',
          letterSpacing: 0.2
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

// Shared input style for neatness
const inputStyle = {
  width: '100%',
  padding: '11px',
  borderRadius: 9,
  border: '1px solid #c4d0eb',
  fontSize: 16,
  marginTop: 4,
  marginBottom: 2,
  boxSizing: 'border-box'
};

export default GridTransformerForm;