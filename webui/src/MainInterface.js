// src/IntermediatePage.js
import React from 'react';
import { useNavigate } from 'react-router-dom';

const toolboxes = [
  {
    label: 'PV System Toolbox',
    route: '/pv-system'
  },
  {
    label: 'PV Inverter Toolbox',
    route: '/pv-inverter'
  },
  {
    label: 'PV Control Toolbox',
    route: '/pv-control'
  },
  {
    label: 'Grid and Transformer Toolbox',
    route: '/grid-transformer'
  },
  {
    label: 'Dynamic Toolbox',
    route: '/pv-dynamic'
  },
  {
    label: 'Component Degradation Toolbox',
    route: '/pv-component-degradation'
  },
  {
    label: 'Mission Profile',
    route: '/mission-profile'
  }
];

const MainInterface = () => {
  const navigate = useNavigate();

  return (
    <div
      style={{
        minHeight: '100vh',
        width: '100%',
        background: 'linear-gradient(135deg, #e0ecff 0%, #f9fafc 100%)',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center'
      }}
    >
      <h2
        style={{
          marginTop: 56,
          marginBottom: 48,
          fontWeight: 700,
          fontSize: 36,
          letterSpacing: 1,
          color: '#2e466c'
        }}
      >
        Select a Toolbox
      </h2>
      <div
        style={{
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fit, minmax(260px, 1fr))',
          gap: 32,
          width: '90%',
          maxWidth: 1000
        }}
      >
        {toolboxes.map(({ label, route }) => (
          <button
            key={label}
            onClick={() => navigate(route)}
            style={{
              background: 'linear-gradient(125deg, #f3f7ff 60%, #e7edfb 100%)',
              border: 'none',
              borderRadius: 20,
              padding: '40px 20px',
              boxShadow: '0 6px 24px 0 rgba(40,90,200,0.10)',
              fontSize: 20,
              fontWeight: 600,
              color: '#355287',
              cursor: 'pointer',
              transition: 'transform 0.16s cubic-bezier(.4,2,.5,.99), box-shadow 0.2s',
              outline: 'none',
              letterSpacing: 0.2,
              position: 'relative'
            }}
            onMouseOver={e => {
              e.currentTarget.style.transform = 'translateY(-4px) scale(1.035)';
              e.currentTarget.style.boxShadow = '0 16px 32px 0 rgba(80,110,220,0.13)';
              e.currentTarget.style.background = 'linear-gradient(125deg, #e4eeff 40%, #e7edfb 100%)';
            }}
            onMouseOut={e => {
              e.currentTarget.style.transform = 'none';
              e.currentTarget.style.boxShadow = '0 6px 24px 0 rgba(40,90,200,0.10)';
              e.currentTarget.style.background = 'linear-gradient(125deg, #f3f7ff 60%, #e7edfb 100%)';
            }}
            onMouseDown={e => {
              e.currentTarget.style.transform = 'scale(0.98)';
            }}
            onMouseUp={e => {
              e.currentTarget.style.transform = 'translateY(-4px) scale(1.035)';
            }}
          >
            {label}
          </button>
        ))}
      </div>
    </div>
  );
};

export default MainInterface;