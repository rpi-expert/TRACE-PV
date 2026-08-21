// src/ControlForm.js
import React, { useState } from 'react';

const defaultPI = { p: '', i: '' };
const defaultBW = '';

function saveFormDataAsJson(formData, filename = "control_config.json") {
  const dataStr = JSON.stringify(formData, null, 2);
  const blob = new Blob([dataStr], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}


const ControlForm = () => {
  const [boostConverterPresent, setBoostConverterPresent] = useState(true);

  // Control which method is used for each section (PI or BW)
  const [dcControlMode, setDcControlMode] = useState('PI');
  const [currentControlMode, setCurrentControlMode] = useState('PI');
  const [qvControlMode, setQvControlMode] = useState('PI');

  const [formData, setFormData] = useState({
    // DC Voltage Controller
    dcPI: { ...defaultPI },
    dcBW: defaultBW,

    // Current Controller
    currentPI: { ...defaultPI },
    currentBW: defaultBW,

    // Q/V Controller (optional)
    qvPI: { ...defaultPI },
    qvBW: defaultBW,

    // DC Voltage Reference
    dcVoltageReference: '1000',

    // MPPT
    initialDutyCycle: '0.66',
    upperDutyCycleLimit: '0.85',
    lowerDutyCycleLimit: '0',
    searchingStepSize: '3e-05',

    // Droop
    pfDroopConstant: '1.667e+05',
    qvDroopConstant: '4167'
  });

  // Handler for main value change
  const handleChange = (e) => {
    const { name, value } = e.target;
    setFormData(f => ({ ...f, [name]: value }));
  };

  // Handler for PI/BW values in nested objects
  const handleNestedChange = (section, key, value) => {
    setFormData(f => ({
      ...f,
      [section]: {
        ...f[section],
        [key]: value
      }
    }));
  };

  // Handler for PI/BW radio selection
  const handleModeChange = (section, mode) => {
    if (section === 'dc') setDcControlMode(mode);
    if (section === 'current') setCurrentControlMode(mode);
    if (section === 'qv') setQvControlMode(mode);
  };

  const handleSubmit = (e) => {
    e.preventDefault();
    saveFormDataAsJson(formData);
    alert('Your configuration was saved as a JSON file!');
    alert('Submitted!\n' + JSON.stringify({ boostConverterPresent, ...formData }, null, 2));
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
      padding: 24
    }}
  >
    <form
      onSubmit={handleSubmit}
      style={{
        background: '#fff',
        borderRadius: 22,
        boxShadow: '0 8px 32px 0 rgba(40,90,200,0.09)',
        padding: '38px 30px',
        minWidth: 340,
        maxWidth: 1100,
        width: '100%',
      }}
    >
      <h2 style={headingStyle}>Control Parameters</h2>
      <div style={{ marginBottom: 18 }}>
        <label style={labelStyle}>
          <input
            type="checkbox"
            checked={boostConverterPresent}
            onChange={e => setBoostConverterPresent(e.target.checked)}
            style={{ marginRight: 9 }}
          />
          Boost Converter Present
        </label>
      </div>
      <div
        style={{
          display: 'grid',
          gridTemplateColumns: '1fr 1fr 1fr',
          gap: 30,
          alignItems: 'start',
          marginBottom: 24,
        }}
      >
        {/* === COLUMN 1 === */}
        <div>
          {/* DC Voltage Controller */}
          <div style={sectionStyle}>
            <div style={sectionHeaderStyle}>
              <h3 style={sectionTitleStyle}>DC Voltage Controller</h3>
              <div>
                <label style={toggleStyle}>
                  <input
                    type="radio"
                    name="dcControlMode"
                    checked={dcControlMode === 'PI'}
                    onChange={() => handleModeChange('dc', 'PI')}
                  /> PI
                </label>
                <label style={toggleStyle}>
                  <input
                    type="radio"
                    name="dcControlMode"
                    checked={dcControlMode === 'BW'}
                    onChange={() => handleModeChange('dc', 'BW')}
                  /> BW
                </label>
              </div>
            </div>
            {dcControlMode === 'PI' ? (
              <div style={flexRowStyle}>
                <input
                  type="text"
                  name="dcPIp"
                  placeholder="P"
                  value={formData.dcPI.p}
                  onChange={e => handleNestedChange('dcPI', 'p', e.target.value)}
                  style={inputStyle}
                />
                <input
                  type="text"
                  name="dcPIi"
                  placeholder="I"
                  value={formData.dcPI.i}
                  onChange={e => handleNestedChange('dcPI', 'i', e.target.value)}
                  style={inputStyle}
                />
              </div>
            ) : (
              <input
                type="text"
                name="dcBW"
                placeholder="Bandwidth"
                value={formData.dcBW}
                onChange={handleChange}
                style={inputStyle}
              />
            )}
            {boostConverterPresent && (
              <input
                type="text"
                name="dcVoltageReference"
                placeholder="DC Voltage Reference"
                value={formData.dcVoltageReference}
                onChange={handleChange}
                style={inputStyle}
              />
            )}
          </div>
        </div>
        {/* === COLUMN 2 === */}
        <div>
          {/* Current Controller */}
          <div style={sectionStyle}>
            <div style={sectionHeaderStyle}>
              <h3 style={sectionTitleStyle}>Current Controller</h3>
              <div>
                <label style={toggleStyle}>
                  <input
                    type="radio"
                    name="currentControlMode"
                    checked={currentControlMode === 'PI'}
                    onChange={() => handleModeChange('current', 'PI')}
                  /> PI
                </label>
                <label style={toggleStyle}>
                  <input
                    type="radio"
                    name="currentControlMode"
                    checked={currentControlMode === 'BW'}
                    onChange={() => handleModeChange('current', 'BW')}
                  /> BW
                </label>
              </div>
            </div>
            {currentControlMode === 'PI' ? (
              <div style={flexRowStyle}>
                <input
                  type="text"
                  name="currentPIp"
                  placeholder="P"
                  value={formData.currentPI.p}
                  onChange={e => handleNestedChange('currentPI', 'p', e.target.value)}
                  style={inputStyle}
                />
                <input
                  type="text"
                  name="currentPIi"
                  placeholder="I"
                  value={formData.currentPI.i}
                  onChange={e => handleNestedChange('currentPI', 'i', e.target.value)}
                  style={inputStyle}
                />
              </div>
            ) : (
              <input
                type="text"
                name="currentBW"
                placeholder="Bandwidth"
                value={formData.currentBW}
                onChange={handleChange}
                style={inputStyle}
              />
            )}
          </div>
          {/* MPPT */}
          <div style={{ ...sectionStyle, border: "none", marginTop: 14 }}>
            <h4 style={{ ...sectionTitleStyle, fontSize: 15, marginTop: 10, marginBottom: 8 }}>MPPT Parameters</h4>
            <div style={flexRowStyle}>
              <input
                type="text"
                name="initialDutyCycle"
                placeholder="Initial Duty Cycle"
                value={formData.initialDutyCycle}
                onChange={handleChange}
                style={inputStyle}
              />
              <input
                type="text"
                name="upperDutyCycleLimit"
                placeholder="Upper Limit"
                value={formData.upperDutyCycleLimit}
                onChange={handleChange}
                style={inputStyle}
              />
            </div>
            <div style={flexRowStyle}>
              <input
                type="text"
                name="lowerDutyCycleLimit"
                placeholder="Lower Limit"
                value={formData.lowerDutyCycleLimit}
                onChange={handleChange}
                style={inputStyle}
              />
              <input
                type="text"
                name="searchingStepSize"
                placeholder="Step Size"
                value={formData.searchingStepSize}
                onChange={handleChange}
                style={inputStyle}
              />
            </div>
          </div>
        </div>
        {/* === COLUMN 3 === */}
        <div>
          {/* Q/V Controller */}
          <div style={sectionStyle}>
            <div style={sectionHeaderStyle}>
              <h3 style={sectionTitleStyle}>Q/V Controller (Optional)</h3>
              <div>
                <label style={toggleStyle}>
                  <input
                    type="radio"
                    name="qvControlMode"
                    checked={qvControlMode === 'PI'}
                    onChange={() => handleModeChange('qv', 'PI')}
                  /> PI
                </label>
                <label style={toggleStyle}>
                  <input
                    type="radio"
                    name="qvControlMode"
                    checked={qvControlMode === 'BW'}
                    onChange={() => handleModeChange('qv', 'BW')}
                  /> BW
                </label>
              </div>
            </div>
            {qvControlMode === 'PI' ? (
              <div style={flexRowStyle}>
                <input
                  type="text"
                  name="qvPIp"
                  placeholder="P"
                  value={formData.qvPI.p}
                  onChange={e => handleNestedChange('qvPI', 'p', e.target.value)}
                  style={inputStyle}
                />
                <input
                  type="text"
                  name="qvPIi"
                  placeholder="I"
                  value={formData.qvPI.i}
                  onChange={e => handleNestedChange('qvPI', 'i', e.target.value)}
                  style={inputStyle}
                />
              </div>
            ) : (
              <input
                type="text"
                name="qvBW"
                placeholder="Bandwidth"
                value={formData.qvBW}
                onChange={handleChange}
                style={inputStyle}
              />
            )}
          </div>
          {/* DROOP CONSTANTS */}
          <div style={{ ...sectionStyle, border: "none", marginTop: 14 }}>
            <h4 style={{ ...sectionTitleStyle, fontSize: 15, marginTop: 10, marginBottom: 8 }}>Droop Constants (Optional)</h4>
            <input
              type="text"
              name="pfDroopConstant"
              placeholder="P/f Droop Constant (D_p)"
              value={formData.pfDroopConstant}
              onChange={handleChange}
              style={inputStyle}
            />
            <input
              type="text"
              name="qvDroopConstant"
              placeholder="Q/V Droop Constant (D_q)"
              value={formData.qvDroopConstant}
              onChange={handleChange}
              style={inputStyle}
            />
          </div>
        </div>
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
          marginTop: 28,
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
        Start Simulation
      </button>
    </form>
  </div>
);
};

// --- STYLES ---
const headingStyle = {
  fontWeight: 700, fontSize: 28, color: '#2e466c', marginBottom: 12, textAlign: 'center'
};
const sectionStyle = {
  marginBottom: 18, paddingBottom: 2, borderBottom: '1px solid #e0e7ef'
};
const sectionHeaderStyle = {
  display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6
};
const sectionTitleStyle = {
  color: '#4973ba', fontWeight: 600, fontSize: 18, margin: 0
};
const labelStyle = {
  fontWeight: 600, fontSize: 16, marginBottom: 4, marginTop: 4, display: 'block', color: '#264672'
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
const flexRowStyle = {
  display: 'flex', gap: 8, marginBottom: 4
};
const toggleStyle = {
  marginLeft: 12, fontWeight: 500, color: '#375481'
};

export default ControlForm;