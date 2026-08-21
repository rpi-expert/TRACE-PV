// src/DynamicForm.js
import React, { useState } from 'react';

const DynamicForm = () => {
  const [form, setForm] = useState({
    lightning: '',
    fault: '',
    gridMaintenance: ''
  });

  const handleChange = e => {
    setForm({
      ...form,
      [e.target.name]: e.target.value
    });
  };

  const handleSubmit = e => {
    e.preventDefault();
    // TODO: Handle form submission logic
    alert('Dynamic Information submitted!\n' + JSON.stringify(form, null, 2));
  };

  return (
    <div
      style={{
        minHeight: '100vh',
        width: '100vw',
        background: 'linear-gradient(135deg, #e0ecff 0%, #f9fafc 100%)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center'
      }}
    >
      <form
        onSubmit={handleSubmit}
        style={{
          background: '#fff',
          borderRadius: 22,
          boxShadow: '0 8px 32px 0 rgba(40,90,200,0.09)',
          padding: '44px 36px',
          minWidth: 320,
          maxWidth: 440,
          width: '100%',
        }}
      >
        <h2 style={{ fontWeight: 700, fontSize: 28, color: '#2e466c', marginBottom: 32 }}>
          Dynamic Events
        </h2>
        <div style={{ marginBottom: 24 }}>
          <label
            htmlFor="lightning"
            style={{ display: 'block', fontWeight: 600, fontSize: 18, marginBottom: 6 }}
          >
            Lightning
          </label>
          <input
            id="lightning"
            name="lightning"
            type="text"
            placeholder="Describe lightning events or frequency"
            value={form.lightning}
            onChange={handleChange}
            style={{
              width: '100%',
              padding: '12px',
              borderRadius: 10,
              border: '1px solid #c4d0eb',
              fontSize: 16,
              marginBottom: 6,
            }}
          />
        </div>
        <div style={{ marginBottom: 24 }}>
          <label
            htmlFor="fault"
            style={{ display: 'block', fontWeight: 600, fontSize: 18, marginBottom: 6 }}
          >
            Fault
          </label>
          <input
            id="fault"
            name="fault"
            type="text"
            placeholder="Describe faults (type, frequency, etc.)"
            value={form.fault}
            onChange={handleChange}
            style={{
              width: '100%',
              padding: '12px',
              borderRadius: 10,
              border: '1px solid #c4d0eb',
              fontSize: 16,
              marginBottom: 6,
            }}
          />
        </div>
        <div style={{ marginBottom: 32 }}>
          <label
            htmlFor="gridMaintenance"
            style={{ display: 'block', fontWeight: 600, fontSize: 18, marginBottom: 6 }}
          >
            Grid Maintenance
          </label>
          <input
            id="gridMaintenance"
            name="gridMaintenance"
            type="text"
            placeholder="Describe grid maintenance schedule or info"
            value={form.gridMaintenance}
            onChange={handleChange}
            style={{
              width: '100%',
              padding: '12px',
              borderRadius: 10,
              border: '1px solid #c4d0eb',
              fontSize: 16,
              marginBottom: 6,
            }}
          />
        </div>
        <button
          type="submit"
          style={{
            width: '100%',
            padding: '16px 0',
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

export default DynamicForm;