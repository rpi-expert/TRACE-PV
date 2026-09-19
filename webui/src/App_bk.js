// src/App.js
// npm install react-router-dom
import React from 'react';
import Tabs from './Tabs';
import { BrowserRouter as Router, Route, Routes, Link } from 'react-router-dom';
import PVSystemForm from './PVSystemForm';
import PVInverterForm from './PVInverterForm';
import ControlForm from './ControlForm';
import WelcomePage from './WelcomePage';
import './App.css';

const App = () => {
  return (
    <Router>
      <div className="App">
        <header className="App-header">
          <h1>TRACE_PV</h1>
          <nav>
            <ul>
              <li><Link to="/">Home</Link></li>
              <li><Link to="/pv-system">PV System</Link></li>
              <li><Link to="/inverter">PV Inverter</Link></li>
              <li><Link to="/control">Control</Link></li>
              <li><Link to="/mission-profile">Mission Profile</Link></li>
              <li><Link to="/progress">Progress</Link></li>
              <li><Link to="/simulation-result">Simulation Result</Link></li>
            </ul>
          </nav>
          <Routes>
            <Route path="/" element={<WelcomePage />} />
            <Route path="/pv-system" element={<PVSystemForm />} />
            <Route path="/inverter" element={<PVInverterForm />} />
            <Route path="/control" element={<ControlForm />} />
            <Route path="/mission-profile" element={
              <div>
                <h2>Mission Profile</h2>
                <p>Mission Profile Content</p>
              </div>
            } />
            <Route path="/progress" element={
              <div>
                <h2>Progress</h2>
                <p>Progress Content</p>
              </div>
            } />
            <Route path="/simulation-result" element={<SimulationResult />} />
          </Routes>
        </header>
      </div>
    </Router>
  );
};

const SimulationResult = () => (
  <div className="simulation-result">
    <h2>Simulation Result</h2>
    <div className="result-section">
      <h3>Capacitor</h3>
      <label>
        IDC:
        <input type="text" />
      </label>
      <label>
        V:
        <input type="text" />
      </label>
      <label>
        T:
        <input type="text" />
      </label>
      <label>
        RH:
        <input type="text" />
      </label>
      <label>
        Ik1_a:
        <input type="text" />
      </label>
      <label>
        Ik1_b:
        <input type="text" />
      </label>
      <label>
        Ik1_c:
        <input type="text" />
      </label>
    </div>
    <div className="result-section">
      <h3>MOV</h3>
      <label>
        Vc_abc:
        <input type="text" />
      </label>
      <label>
        Vpcc_abc:
        <input type="text" />
      </label>
      <label>
        Vgrid_abc:
        <input type="text" />
      </label>
      <label>
        Ik2_a:
        <input type="text" />
      </label>
      <label>
        Ik2_b:
        <input type="text" />
      </label>
      <label>
        Ik2_c:
        <input type="text" />
      </label>
    </div>
  </div>
);

export default App;
