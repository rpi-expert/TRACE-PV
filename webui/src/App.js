import React, { useEffect, useState } from 'react';
import { BrowserRouter as Router, NavLink, Route, Routes } from 'react-router-dom';
import axios from 'axios';
import ComponentDegradation from './toolboxes/ComponentDegradation';
import WorkflowToolboxForm from './WorkflowToolboxForm';
import WorkflowInputCollection from './WorkflowInputCollection';
import { readWorkflowInputs } from './workflowSchema';
import './App.css';

const defaultForm = {
  topology: '3l2s',
  inputMode: 'mission',
  staticTemp: 25,
  staticRh: 85,
  staticVoltage: 500,
  staticPower: 300,
  staticIrradiance: 1000,
  staticCases: 105120,
  missionCsv: '',
  environmentalCsv: 'simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv',
  operatingCsv: 'simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv',
  rounds: 1,
  // 0 keeps the original lifetime logic: simulate the one-year electrical
  // profile once, then reuse it for reliability iterations until failure.
  maxIterations: 0,
  postProcessingMode: 'reference',
  thermalStep: 0,
  modulation: 'svm',
  ngpus: 'all',
  model: 'simulator_inputs/simulation_model/example_simulation_model.json',
};

const numberFields = new Set([
  'staticTemp',
  'staticRh',
  'staticVoltage',
  'staticPower',
  'staticIrradiance',
  'rounds',
]);

const toolboxLinks = [
  { key: 'pvSystem', label: 'PV System', path: '/pv-system' },
  { key: 'pvInverter', label: 'PV Inverter', path: '/pv-inverter' },
  { key: 'control', label: 'PV Control', path: '/pv-control' },
  { key: 'gridTransformer', label: 'Grid & Transformer', path: '/grid-transformer' },
  { key: 'dynamicEvents', label: 'Dynamic Events', path: '/pv-dynamic' },
  { key: 'missionProfile', label: 'Mission Profile', path: '/mission-profile' },
  { key: 'powerModule', label: 'Power Module', path: '/component/power-module' },
  { key: 'capacitor', label: 'Capacitor', path: '/component/capacitor' },
  { key: 'pcb', label: 'PCB', path: '/component/pcb' },
  { key: 'coolingFan', label: 'Cooling Fan', path: '/component/cooling-fan' },
  { key: 'mov', label: 'MOV', path: '/component/movs' },
  { key: 'relay', label: 'Relay', path: '/component/relay' },
];

const degradationItems = [
  { key: 'fan_electrical_external', label: 'Cooling Fan (Considering Electriacl Failure Mounted Outside)', chartLabel: 'Fan · Electrical · Outside' },
  { key: 'fan_electrical_internal', label: 'Cooling Fan (Considering Electriacl Failure Mounted Inside)', chartLabel: 'Fan · Electrical · Inside' },
  { key: 'fan_mechanical_external', label: 'Cooling Fan (Considering Mechanical Failure Mounted Outside)', chartLabel: 'Fan · Mechanical · Outside' },
  { key: 'fan_mechanical_internal', label: 'Cooling Fan (Considering Mechanical Failure Mounted Inside)', chartLabel: 'Fan · Mechanical · Inside' },
  { key: 'capacitor', label: 'Capacitor Considering Voltage and Temeprature Streesors', chartLabel: 'Capacitor · V & T' },
  { key: 'igbt_delta_t', label: 'Power Module Considering deltaT and Tmax', chartLabel: 'Power Module · ΔT & Tmax' },
  { key: 'igbt_arrhenius', label: 'Power Module Considering RH,T, Volt', chartLabel: 'Power Module · RH, T & V' },
  { key: 'pcb', label: 'PCB Considering Temperature and Temeparture varianc', chartLabel: 'PCB · T & T Variance' },
];

function statusText(status) {
  return {
    idle: 'Idle',
    queued: 'Queued',
    running: 'Running',
    completed: 'Completed',
    failed: 'Failed',
    stopped: 'Stopped',
  }[status] || status;
}

function fieldValue(value, unit = '') {
  if (value === undefined || value === null || value === '') return '-';
  if (Array.isArray(value)) return `${value.map((item) => Number(item).toFixed(2)).join(' to ')}${unit}`;
  if (unit === ' %') return `${Number(value).toFixed(2)}${unit}`;
  if (Number.isInteger(value)) return `${value}${unit}`;
  if (typeof value === 'number') return `${Number(value).toExponential(3)}${unit}`;
  return `${value}${unit}`;
}

function formatDuration(value) {
  const seconds = Number(value);
  if (!Number.isFinite(seconds)) return '-';
  const totalSeconds = Math.max(0, Math.floor(seconds));
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const secs = totalSeconds % 60;
  return `${String(hours).padStart(2, '0')}h:${String(minutes).padStart(2, '0')}m:${String(secs).padStart(2, '0')}s`;
}

function degradationPercent(value) {
  const clamped = Math.min(Math.max(Number(value || 0), 0), 1);
  return `${(clamped * 100).toFixed(2)}%`;
}

function thermalActionText(action) {
  return {
    initial: 'Initial one-year run',
    updated: 'Updated thermal parameters',
    rerun: 'Critical-degradation rerun',
    reuse: 'Cached reuse',
  }[action] || 'Waiting';
}

function Metric({ label, value, unit, formatter }) {
  return (
    <div className="metric">
      <span>{label}</span>
      <strong>{formatter ? formatter(value) : fieldValue(value, unit)}</strong>
    </div>
  );
}

function DegradationBars({ degradation = {} }) {
  return (
    <div className="degradation-bars">
      {degradationItems.map((item) => {
        const value = Number(degradation[item.key] || 0);
        const percent = Math.min(Math.max(value, 0), 1) * 100;
        return (
          <div className="degradation-row" key={item.key}>
            <div className="degradation-label">
              <span>{item.label}</span>
              <strong>{degradationPercent(value)}</strong>
            </div>
            <div className="degradation-track">
              <div style={{ width: `${percent}%` }} />
            </div>
          </div>
        );
      })}
    </div>
  );
}

function clamp01(value) {
  return Math.min(Math.max(Number(value || 0), 0), 1);
}

function timeYearsForPoint(point, index) {
  const explicit = Number(point.time_years);
  if (Number.isFinite(explicit) && explicit >= 0) return explicit;
  const iteration = Math.max(Number(point.iteration || 1), 1);
  const round = Number(point.round || 0);
  const roundTotal = Number(point.round_total || 0);
  if (round > 0 && roundTotal > 0) return (iteration - 1) + (round / roundTotal);
  return Number.isFinite(iteration) ? iteration : index + 1;
}

function formatYearTick(value) {
  if (value === 0) return '0';
  if (value < 1) return `${Math.round(value * 12)} mo`;
  if (Math.abs(value - Math.round(value)) < 0.01) return `${Math.round(value)} yr`;
  return `${value.toFixed(1)} yr`;
}

function buildYearTicks(maxYear) {
  if (maxYear <= 1) return [0, 1 / 6, 1 / 3, 0.5, 2 / 3, 5 / 6, 1];
  if (maxYear <= 3) return [0, 1, 2, 3].filter((tick) => tick <= maxYear);
  if (maxYear <= 5) return [0, 1, 3, 5].filter((tick) => tick <= maxYear);
  if (maxYear <= 10) return [0, 3, 5, 10].filter((tick) => tick <= maxYear);
  const ticks = [0, 3, 5, 10];
  const step = maxYear <= 30 ? 5 : 10;
  for (let tick = 15; tick < maxYear; tick += step) ticks.push(tick);
  ticks.push(Math.ceil(maxYear / step) * step);
  return Array.from(new Set(ticks)).filter((tick) => tick <= Math.ceil(maxYear / step) * step);
}

function DegradationHistory({ history = [] }) {
  const points = history
    .map((point, index) => ({ ...point, timeYears: timeYearsForPoint(point, index) }))
    .filter((point) => Number.isFinite(point.timeYears));

  if (!points.length) {
    return <div className="empty-chart">No accumulated degradation samples yet.</div>;
  }

  const width = 720;
  const height = 270;
  const margin = { top: 54, right: 16, bottom: 34, left: 44 };
  const plotWidth = width - margin.left - margin.right;
  const plotHeight = height - margin.top - margin.bottom;
  const minYear = 0;
  const maxYear = Math.max(1, ...points.map((point) => point.timeYears));
  const xTicks = buildYearTicks(maxYear);
  const yTicks = [0, 0.25, 0.5, 0.75, 1];
  const xFor = (timeYears) => margin.left + ((timeYears - minYear) / (maxYear - minYear || 1)) * plotWidth;
  const yFor = (value) => margin.top + plotHeight - clamp01(value) * plotHeight;

  return (
    <svg className="history-chart" viewBox={`0 0 ${width} ${height}`} role="img" aria-label="Accumulated degradation history">
      <g className="history-legend">
        {degradationItems.map((item, itemIndex) => {
          const columns = 4;
          const columnWidth = 160;
          const rowHeight = 16;
          const x = margin.left + (itemIndex % columns) * columnWidth;
          const y = 18 + Math.floor(itemIndex / columns) * rowHeight;
          return (
            <g key={`legend-${item.key}`} transform={`translate(${x},${y})`}>
              <line className={`series-${itemIndex}`} x1="0" y1="0" x2="18" y2="0" />
              <circle className={`series-dot series-${itemIndex}`} cx="9" cy="0" r="2.4" />
              <text x="24" y="4">{item.chartLabel || item.label}</text>
            </g>
          );
        })}
      </g>
      {yTicks.map((tick) => (
        <g key={`y-${tick}`}>
          <line className="history-grid" x1={margin.left} y1={yFor(tick)} x2={width - margin.right} y2={yFor(tick)} />
          <text className="history-y-label" x={margin.left - 9} y={yFor(tick) + 4}>{tick.toFixed(tick === 0 || tick === 1 ? 0 : 2)}</text>
        </g>
      ))}
      {xTicks.map((tick) => (
        <g key={`x-${tick}`}>
          <line className="history-grid" x1={xFor(tick)} y1={margin.top} x2={xFor(tick)} y2={height - margin.bottom} />
          <text className="history-x-label" x={xFor(tick)} y={height - 12}>{formatYearTick(tick)}</text>
        </g>
      ))}
      <line className="history-axis" x1={margin.left} y1={margin.top} x2={margin.left} y2={height - margin.bottom} />
      <line className="history-axis" x1={margin.left} y1={height - margin.bottom} x2={width - margin.right} y2={height - margin.bottom} />
      {degradationItems.map((item, itemIndex) => {
        const linePoints = points.map((point) => `${xFor(point.timeYears)},${yFor(point.degradation?.[item.key])}`).join(' ');
        return (
          <g key={item.key}>
            <polyline className={`series series-${itemIndex}`} points={linePoints} />
            {points.length <= 60 && points.map((point) => (
              <circle
                key={`${item.key}-${point.iteration}-${point.round || point.timeYears}`}
                className={`series-dot series-${itemIndex}`}
                cx={xFor(point.timeYears)}
                cy={yFor(point.degradation?.[item.key])}
                r="2.2"
              />
            ))}
          </g>
        );
      })}
      <text className="history-axis-title" x={margin.left + plotWidth / 2} y={height - 2}>Time</text>
    </svg>
  );
}

function ParameterCollectionPanel() {
  return <section className="panel parameter-panel"><WorkflowInputCollection compact /></section>;
}

function InverterIllustrationPanel() {
  return (
    <section className="panel inverter-illustration" aria-labelledby="inverter-illustration-title">
      <div className="section-title inverter-illustration-title">
        <div>
          <h2 id="inverter-illustration-title">Inverter Components</h2>
          <p>Cooling fan, IGBT power module, and DC-link capacitor</p>
        </div>
      </div>
      <img
        src="/assets/inverter-components-cartoon.png"
        alt="Cartoon cutaway of an inverter enclosure showing the cooling fan, IGBT power module, and capacitor"
      />
    </section>
  );
}

function SimulatorDashboard() {
  const [form, setForm] = useState(defaultForm);
  const [jobId, setJobId] = useState(() => new URLSearchParams(window.location.search).get('job') || '');
  const [job, setJob] = useState({ status: 'idle', progress: 0, result: {} });
  const [error, setError] = useState('');

  const isRunning = job.status === 'running' || job.status === 'queued';
  const isTerminal = ['completed', 'failed', 'stopped'].includes(job.status);
  const result = job.result || {};
  const degradation = result.degradation || {};
  const activeYear = isTerminal ? '-' : (result.current_year || job.iteration || '-');
  const activeRound = isTerminal
    ? '-'
    : (result.round_total
      ? `${result.current_round || 0} / ${result.round_total}`
      : (result.current_round || '-'));

  useEffect(() => {
    if (!jobId) return undefined;
    let active = true;
    const load = async () => {
      try {
        const jobRes = await axios.get(`/api/simulations/${jobId}`);
        if (!active) return;
        setJob(jobRes.data);
        setError('');
      } catch (err) {
        if (!active) return;
        if (err.response?.status === 404) {
          const cleanUrl = new URL(window.location.href);
          cleanUrl.searchParams.delete('job');
          window.history.replaceState({}, '', cleanUrl);
          setJobId('');
          setJob({ status: 'idle', progress: 0, result: {} });
          setError('');
          return;
        }
        setError(err.response?.data?.error || err.message);
      }
    };
    load();
    const timer = setInterval(load, isRunning ? 1500 : 4000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [jobId, isRunning]);

  function updateField(event) {
    const { name, value } = event.target;
    setForm((current) => ({
      ...current,
      [name]: numberFields.has(name) ? Number(value) : value,
    }));
  }

  async function startSimulation(event) {
    event.preventDefault();
    setError('');
    const toolboxInputs = readWorkflowInputs();
    const missing = toolboxLinks.filter((item) => item.key && !toolboxInputs[item.key]).map((item) => item.label);
    if (missing.length) {
      setError(`Complete the workflow input collection first: ${missing.join(', ')}`);
      return;
    }
    try {
      const mission = toolboxInputs.missionProfile || {};
      const inverter = toolboxInputs.pvInverter || {};
      const payload = {
        ...form,
        environmentalCsv: mission.environmentalCsv || form.environmentalCsv,
        operatingCsv: mission.operatingCsv || form.operatingCsv,
        modulation: inverter.inverterModulationScheme?.toLowerCase() || form.modulation,
        toolboxInputs,
      };
      const response = await axios.post('/api/simulations', payload);
      setJobId(response.data.id);
      const monitorUrl = new URL(window.location.href);
      monitorUrl.searchParams.set('job', response.data.id);
      window.history.replaceState({}, '', monitorUrl);
      setJob(response.data);
    } catch (err) {
      setError(err.response?.data?.error || err.message);
    }
  }

  async function stopSimulation() {
    if (!jobId) return;
    await axios.post(`/api/simulations/${jobId}/stop`);
  }

  return (
    <main className="shell">
      <header className="topbar">
        <div>
          <h1>TRACE-PV Simulator</h1>
          <p>Input control, accumulated degradation progress, and lifetime result review</p>
        </div>
        <div className={`status-pill ${job.status}`}>
          <span />
          {statusText(job.status)}
        </div>
      </header>

      {error && <div className="alert">{error}</div>}

      <section className="workspace">
        <div className="left-stack">
          <form className="panel controls" id="simulator-input" onSubmit={startSimulation}>
            <div className="section-title">
              <h2>Simulator Input</h2>
              <button type="submit" disabled={isRunning}>{isRunning ? 'Running' : 'Run'}</button>
              <button type="button" className="secondary" disabled={!isRunning} onClick={stopSimulation}>Stop</button>
            </div>

            <div className="grid two">
              <label>
                Topology
                <select name="topology" value={form.topology} onChange={updateField}>
                  <option value="2l2s">2L2S</option>
                  <option value="2l1s">2L1S</option>
                  <option value="3l2s">3L2S</option>
                  <option value="3l1s">3L1S</option>
                </select>
              </label>
              <label>
                Input mode
                <select name="inputMode" value={form.inputMode} onChange={updateField}>
                  <option value="static">Static</option>
                  <option value="mission">Mission Profile</option>
                </select>
              </label>
              <label>
                Rounds (batches per year)
                <input
                  name="rounds"
                  type="number"
                  min="1"
                  title="The one-year profile is divided into this many sequential batches; 6 rounds means 2 months per batch."
                  value={form.rounds}
                  onChange={updateField}
                />
              </label>
              <label>
                Modulation
                <select name="modulation" value={form.modulation} onChange={updateField}>
                  <option value="svm">SVM</option>
                  <option value="spwm">SPWM</option>
                </select>
              </label>
              <label>
                GPUs
                <input name="ngpus" value={form.ngpus} onChange={updateField} />
              </label>
            </div>

            {form.inputMode === 'static' ? (
              <div className="grid two">
                <label>
                  T (C)
                  <input name="staticTemp" type="number" value={form.staticTemp} onChange={updateField} />
                </label>
                <label>
                  RH (%)
                  <input name="staticRh" type="number" min="0" max="100" value={form.staticRh} onChange={updateField} />
                </label>
                <label>
                  AC voltage (V)
                  <input name="staticVoltage" type="number" value={form.staticVoltage} onChange={updateField} />
                </label>
                <label>
                  AC power (W)
                  <input name="staticPower" type="number" value={form.staticPower} onChange={updateField} />
                </label>
                <label>
                  Irradiance (W/m2)
                  <input name="staticIrradiance" type="number" value={form.staticIrradiance} onChange={updateField} />
                </label>
              </div>
            ) : null}
          </form>

          <ParameterCollectionPanel />
          <InverterIllustrationPanel />
        </div>

        <section className="panel progress">
          <div className="section-title">
            <h2>Progress</h2>
            <span>{statusText(job.status)}</span>
          </div>
          <div className="grid two metrics-grid">
            <Metric label="Job ID" value={jobId || '-'} />
            <Metric label="Exit code" value={job.exit_code ?? '-'} />
            <Metric label="Active year / iteration" value={activeYear} />
            <Metric label="Completed years" value={result.simulated_years ?? 0} />
            <Metric label="Active round" value={activeRound} />
            <Metric label="Completed round" value={result.round_total ? `${result.completed_round || 0} / ${result.round_total}` : (result.completed_round || '-')} />
            <Metric label="Thermal simulation" value={thermalActionText(result.thermal_action)} />
            <Metric label="Thermal reruns" value={result.thermal_reruns ?? 0} />
            <Metric label="Wall duration" value={result.duration_seconds} formatter={formatDuration} />
          </div>
          <h2 className="subhead" id="degradation-results">Accumulated Degradation</h2>
          <DegradationBars degradation={degradation} />
          <DegradationHistory history={result.degradation_history || []} />
        </section>
      </section>

    </main>
  );
}

function AppFrame() {
  return (
    <>
      <nav className="app-nav">
        <NavLink to="/simulator">Simulator</NavLink>
        <NavLink to="/main-gui">Parameter Collection</NavLink>
      </nav>
      <Routes>
        <Route path="/" element={<SimulatorDashboard />} />
        <Route path="/simulator" element={<SimulatorDashboard />} />
        <Route path="/main-gui" element={<main className="shell"><section className="panel parameter-panel"><WorkflowInputCollection /></section></main>} />
        <Route path="/pv-system" element={<WorkflowToolboxForm toolboxKey="pvSystem" />} />
        <Route path="/pv-inverter" element={<WorkflowToolboxForm toolboxKey="pvInverter" />} />
        <Route path="/pv-control" element={<WorkflowToolboxForm toolboxKey="control" />} />
        <Route path="/grid-transformer" element={<WorkflowToolboxForm toolboxKey="gridTransformer" />} />
        <Route path="/pv-dynamic" element={<WorkflowToolboxForm toolboxKey="dynamicEvents" />} />
        <Route path="/pv-component-degradation" element={<ComponentDegradation />} />
        <Route path="/mission-profile" element={<WorkflowToolboxForm toolboxKey="missionProfile" />} />
        <Route path="/component" element={<ComponentDegradation />} />
        <Route path="/component/capacitor" element={<WorkflowToolboxForm toolboxKey="capacitor" />} />
        <Route path="/component/movs" element={<WorkflowToolboxForm toolboxKey="mov" />} />
        <Route path="/component/relay" element={<WorkflowToolboxForm toolboxKey="relay" />} />
        <Route path="/component/power-module" element={<WorkflowToolboxForm toolboxKey="powerModule" />} />
        <Route path="/component/cooling-fan" element={<WorkflowToolboxForm toolboxKey="coolingFan" />} />
        <Route path="/component/pcb" element={<WorkflowToolboxForm toolboxKey="pcb" />} />
      </Routes>
    </>
  );
}

function App() {
  return (
    <Router>
      <AppFrame />
    </Router>
  );
}

export default App;
