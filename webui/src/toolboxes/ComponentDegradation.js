import React from 'react';
import { useNavigate } from 'react-router-dom';
// import { Routes, Route } from "react-router-dom";

const components = [
  { name: "Capacitor", route: "capacitor" },
  { name: "MOVs", route: "movs" },
  { name: "Relay", route: "relay" },
  { name: "Power Module", route: "power-module" },
  { name: "Cooling Fan", route: "cooling-fan" },
  { name: "PCB", route: "pcb" },
];

const ComponentDegradation = () => {
  const navigate = useNavigate();
  return (
    <div style={{
      minHeight: "100vh",
      background: "linear-gradient(135deg,#e0ecff 0%,#f9fafc 100%)",
      display: "flex",
      flexDirection: "column",
      alignItems: "center",
      justifyContent: "flex-start",
      paddingTop: 50,
    }}>
      <h2 style={{ fontWeight: 700, fontSize: 32, color: "#2e466c", marginBottom: 28 }}>
        Component Degradation
      </h2>
      <div style={{
        display: "grid",
        gridTemplateColumns: "repeat(auto-fit, minmax(220px, 1fr))",
        gap: 28,
        width: "80%",
        maxWidth: 880
      }}>
        {components.map(({ name, route }) => (
          <div
            key={route}
            onClick={() => navigate(`/component/${route}`)}
            style={{
              background: "#fff",
              borderRadius: 18,
              boxShadow: "0 6px 22px 0 rgba(40,90,200,0.07)",
              padding: "40px 20px",
              display: "flex",
              flexDirection: "column",
              alignItems: "center",
              cursor: "pointer",
              fontWeight: 600,
              fontSize: 20,
              color: "#264672",
              transition: "box-shadow 0.15s, transform 0.13s",
            }}
            onMouseOver={e => {
              e.currentTarget.style.boxShadow = "0 12px 32px 0 rgba(80,110,220,0.13)";
              e.currentTarget.style.transform = "translateY(-3px) scale(1.04)";
            }}
            onMouseOut={e => {
              e.currentTarget.style.boxShadow = "0 6px 22px 0 rgba(40,90,200,0.07)";
              e.currentTarget.style.transform = "none";
            }}
          >
            {name}
          </div>
        ))}
      </div>
    </div>
  );
};

export default ComponentDegradation;

