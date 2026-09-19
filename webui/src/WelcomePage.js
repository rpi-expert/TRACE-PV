// src/WelcomePage.js
import React from 'react';
import { useNavigate } from 'react-router-dom';

const WelcomePage = () => {
  const navigate = useNavigate();

  const handleStartClick = () => {
    navigate('/main-gui'); // Navigate to the Intermediate Page
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
      <div
        style={{
          background: 'rgba(255,255,255,0.98)',
          boxShadow: '0 8px 32px 0 rgba(40,90,200,0.11)',
          borderRadius: 28,
          padding: '56px 40px',
          textAlign: 'center',
          minWidth: 350,
          maxWidth: 420,
        }}
      >
        <h1
          style={{
            fontSize: 40,
            fontWeight: 800,
            color: '#29509f',
            letterSpacing: 1,
            marginBottom: 18,
          }}
        >
          Welcome to <span style={{ color: '#4982ff' }}>TRACE PV</span>
        </h1>
        <p
          style={{
            fontSize: 20,
            color: '#355287',
            marginBottom: 36,
            fontWeight: 500,
            letterSpacing: 0.2
          }}
        >
          PV Inverter Reliability Assessment Tool
        </p>
        <button
          onClick={handleStartClick}
          style={{
            padding: '18px 0',
            width: '65%',
            background: 'linear-gradient(90deg,#4882ff 60%, #76b5ff 100%)',
            color: '#fff',
            fontSize: 22,
            fontWeight: 700,
            border: 'none',
            borderRadius: 14,
            boxShadow: '0 4px 12px 0 rgba(80,110,220,0.14)',
            cursor: 'pointer',
            letterSpacing: 0.5,
            transition: 'background 0.2s, transform 0.13s',
          }}
          onMouseOver={e => {
            e.currentTarget.style.background = 'linear-gradient(90deg, #356fe7 60%, #64a3f8 100%)';
            e.currentTarget.style.transform = 'translateY(-2px) scale(1.04)';
          }}
          onMouseOut={e => {
            e.currentTarget.style.background = 'linear-gradient(90deg, #4882ff 60%, #76b5ff 100%)';
            e.currentTarget.style.transform = 'none';
          }}
          onMouseDown={e => {
            e.currentTarget.style.transform = 'scale(0.98)';
          }}
          onMouseUp={e => {
            e.currentTarget.style.transform = 'translateY(-2px) scale(1.04)';
          }}
        >
          Start
        </button>
      </div>
    </div>
  );
};

export default WelcomePage;