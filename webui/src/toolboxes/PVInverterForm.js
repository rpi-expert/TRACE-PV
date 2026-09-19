import React, { useState } from 'react';

function saveFormDataAsJson(formData, filename = "pv_inverter_config.json") {
  const dataStr = JSON.stringify(formData, null, 2);
  const blob = new Blob([dataStr], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}


const PVInverterForm = () => {
  const [formData, setFormData] = useState({
    boostTopology: 'Present',
    inputCapacitance: '5e-05',
    inductance: '0.005',
    boostSwitchingFrequency: '5000',
    inverterTopology: 'Two-level',
    dcLinkCapacitance: '0.01035',
    modulationScheme: 'SVPWM',
    filterInductanceL1: '0.0005953',
    filterInductanceL2: '0.0001584',
    filterCapacitance: '5.757e-05',
    inverterSwitchingFrequency: '5000'
  });

  const handleChange = (e) => {
    const { name, value } = e.target;
    setFormData({ ...formData, [name]: value });
  };

  const handleSubmit = (e) => {
    e.preventDefault();
    saveFormDataAsJson(formData);
    alert('Your configuration was saved as a JSON file!');
    alert('Form Submitted!\n' + JSON.stringify(formData, null, 2));
  };

  const showBoostFields = formData.boostTopology === 'Present';

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
          padding: '40px 32px',
          minWidth: 380,
          maxWidth: 850,
          width: '100%',
        }}
      >
        <h2 style={{ fontWeight: 700, fontSize: 28, color: '#2e466c', marginBottom: 24, textAlign: 'center' }}>
          PV Inverter Setup
        </h2>
        <div
          style={{
            display: 'grid',
            gridTemplateColumns: '1fr 1fr',
            gap: '32px',
            alignItems: 'start',
          }}
        >
          {/* BOOST CONVERTER COLUMN */}
          <div>
            <h3 style={{ color: '#4973ba', fontWeight: 600, fontSize: 21, marginBottom: 16 }}>
              Boost Converter
            </h3>
            <div style={rowStyle}>
              <label style={labelStyle}>Topology</label>
              <select
                name="boostTopology"
                value={formData.boostTopology}
                onChange={handleChange}
                style={inputStyle}
              >
                <option value="Present">Boost Converter Present</option>
                <option value="Not Present">Boost Converter Not Present</option>
              </select>
            </div>
            {showBoostFields && (
              <>
                <div style={rowStyle}>
                  <label style={labelStyle}>Input Capacitance (C<sub>PV</sub>)</label>
                  <input
                    type="text"
                    name="inputCapacitance"
                    value={formData.inputCapacitance}
                    onChange={handleChange}
                    style={inputStyle}
                  />
                </div>
                <div style={rowStyle}>
                  <label style={labelStyle}>Inductance (L<sub>boost</sub>)</label>
                  <input
                    type="text"
                    name="inductance"
                    value={formData.inductance}
                    onChange={handleChange}
                    style={inputStyle}
                  />
                </div>
                <div style={rowStyle}>
                  <label style={labelStyle}>Switching Frequency (f<sub>boost_sw</sub>)</label>
                  <input
                    type="text"
                    name="boostSwitchingFrequency"
                    value={formData.boostSwitchingFrequency}
                    onChange={handleChange}
                    style={inputStyle}
                  />
                </div>
              </>
            )}
          </div>
          {/* PV INVERTER COLUMN */}
          <div>
            <h3 style={{ color: '#4973ba', fontWeight: 600, fontSize: 21, marginBottom: 16 }}>
              PV Inverter
            </h3>
            <div style={rowStyle}>
              <label style={labelStyle}>Topology</label>
              <select
                name="inverterTopology"
                value={formData.inverterTopology}
                onChange={handleChange}
                style={inputStyle}
              >
                <option value="Two-level">Two-level</option>
                <option value="NPC">NPC</option>
                <option value="ANPC">ANPC</option>
                <option value="T-Type">T-Type</option>
              </select>
            </div>
            <div style={rowStyle}>
              <label style={labelStyle}>DC Link Capacitance (C<sub>DC</sub>)</label>
              <input
                type="text"
                name="dcLinkCapacitance"
                value={formData.dcLinkCapacitance}
                onChange={handleChange}
                style={inputStyle}
              />
            </div>
            <div style={rowStyle}>
              <label style={labelStyle}>Modulation Scheme</label>
              <select
                name="modulationScheme"
                value={formData.modulationScheme}
                onChange={handleChange}
                style={inputStyle}
              >
                <option value="SPWM">SPWM</option>
                <option value="SVPWM">SVPWM</option>
              </select>
            </div>
            <div style={rowStyle}>
              <label style={labelStyle}>Filter Inductance (L1)</label>
              <input
                type="text"
                name="filterInductanceL1"
                value={formData.filterInductanceL1}
                onChange={handleChange}
                style={inputStyle}
              />
            </div>
            <div style={rowStyle}>
              <label style={labelStyle}>Filter Inductance (L2)</label>
              <input
                type="text"
                name="filterInductanceL2"
                value={formData.filterInductanceL2}
                onChange={handleChange}
                style={inputStyle}
              />
            </div>
            <div style={rowStyle}>
              <label style={labelStyle}>Filter Capacitance (C)</label>
              <input
                type="text"
                name="filterCapacitance"
                value={formData.filterCapacitance}
                onChange={handleChange}
                style={inputStyle}
              />
            </div>
            <div style={rowStyle}>
              <label style={labelStyle}>Switching Frequency (f<sub>inv_sw</sub>)</label>
              <input
                type="text"
                name="inverterSwitchingFrequency"
                value={formData.inverterSwitchingFrequency}
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
            marginTop: 38,
            marginBottom: 0,
            display: 'block',
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

const labelStyle = {
  fontWeight: 600,
  fontSize: 16,
  marginBottom: 6,
  marginTop: 6,
  display: 'block',
  color: '#264672'
};

const rowStyle = {
  marginBottom: 16
};

export default PVInverterForm;