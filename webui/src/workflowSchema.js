export const WORKFLOW_STORAGE_KEY = 'tracePvWorkflowInputs';

const number = (name, label, unit, description, extra = {}) => ({
  name, label, type: 'number', unit, description, required: true, ...extra,
});
const text = (name, label, unit, description, extra = {}) => ({
  name, label, type: 'text', unit, description, required: true, ...extra,
});
const select = (name, label, options, description, extra = {}) => ({
  name, label, type: 'select', unit: 'N/A', options, description, required: true, ...extra,
});
const output = (name, type, unit, description) => ({ name, type, unit, description });

export const workflowToolboxes = {
  missionProfile: {
    title: 'Mission Profile', section: '1.1',
    fields: [
      select('sourceMode', 'Mission-profile source', ['NSRDB', 'User CSV'], 'Environmental-condition source.', { defaultValue: 'User CSV' }),
      number('latitude', 'Latitude', '°', 'Latitude of the target location.', { min: -90, max: 90, defaultValue: 39.9042 }),
      number('longitude', 'Longitude', '°', 'Longitude of the target location.', { min: -180, max: 180, defaultValue: 116.4074 }),
      number('year', 'Year', 'N/A', 'Calendar year of the record.', { min: 1998, step: 1, defaultValue: 2024 }),
      number('interval', 'Interval', 'min', 'Time resolution used for an NSRDB request.', { min: 1, step: 1, defaultValue: 5 }),
      text('environmentalCsv', 'Environmental-condition CSV', 'CSV', 'CSV containing IR, Tamb, and RH.', { enabledWhen: { sourceMode: 'User CSV' }, defaultValue: 'simulator_inputs/mission_profile/environmental_condition/environmental_mission_profile.csv' }),
      text('operatingCsv', 'Grid operating-condition CSV', 'CSV', 'CSV containing Vac and PF.', { defaultValue: 'simulator_inputs/mission_profile/operating_condition/operating_mission_profile.csv' }),
    ],
    outputs: [
      output('IR', 'Float[]', 'W/m²', 'Solar irradiance time series.'),
      output('Tamb', 'Float[]', '°F', 'Ambient-temperature time series.'),
      output('RH', 'Float[]', '%', 'Relative-humidity time series.'),
      output('Vac', 'Float[]', 'V', 'AC-voltage operating condition.'),
      output('PF', 'Float[]', 'N/A', 'Power-factor operating condition.'),
    ],
  },
  pvSystem: {
    title: 'PV System', section: '1.2',
    fields: [
      text('pvModulePartNumber', 'PV module part number', 'N/A', 'PV module used in the system.', { defaultValue: 'CS6U-330P' }),
      number('numberOfParallelStrings', 'Number of parallel strings', 'N/A', 'Parallel-connected strings in the array.', { min: 1, step: 1, defaultValue: 66 }),
      number('numberOfSeries', 'Modules in series per string', 'N/A', 'Series-connected modules in each string.', { min: 1, step: 1, defaultValue: 5 }),
    ],
    outputs: [output('pvIvCurve', 'Float[]', 'A/V', 'Configured PV-string I–V curve.'), output('pvPVCurve', 'Float[]', 'W/V', 'PV output power-versus-voltage curve.')],
  },
  gridTransformer: {
    title: 'Grid and Transformer', section: '1.2.1.2–1.2.1.3',
    fields: [
      number('gridVoltage', 'Grid voltage', 'V', 'Nominal RMS line-to-line voltage.', { defaultValue: 277.128129 }),
      number('gridFrequency', 'Grid frequency', 'Hz', 'Nominal grid frequency.', { min: 0, defaultValue: 60 }),
      number('gridImpedance', 'Grid impedance', 'Ω', 'Equivalent impedance at the inverter connection point.', { min: 0, defaultValue: 0 }),
      number('transformerPrimaryVoltage', 'Transformer primary voltage', 'kV', 'Primary-side voltage rating.', { min: 0, defaultValue: 12.47 }),
      number('transformerSecondaryVoltage', 'Transformer secondary voltage', 'V', 'Secondary-side voltage rating.', { min: 0, defaultValue: 480 }),
      select('transformerConfiguration', 'Transformer configuration', ['Y-grounded/Y-grounded', 'Delta/Y-grounded', 'Y/Delta', 'Delta/Delta'], 'Transformer winding configuration.', { defaultValue: 'Y-grounded/Y-grounded' }),
      number('transformerPowerRatings', 'Transformer power rating', 'MVA', 'Transformer rated apparent power.', { min: 0, defaultValue: 0.03 }),
    ], outputs: [],
  },
  pvInverter: {
    title: 'PV Inverter', section: '1.3',
    fields: [
      select('pvInverterConfiguration', 'Boost converter included', ['true', 'false'], 'Whether a boost-converter stage is present.', { defaultValue: 'true' }),
      text('pvInverterPartNumber', 'PV inverter part number', 'N/A', 'Identification code of the PV inverter.', { defaultValue: 'SMA_Sunny_Tripower_30000TL-US-10' }),
      select('pvInverterTopology', 'Inverter topology', ['Two-level', 'NPC', 'ANPC', 'T-type'], 'Power-stage topology.', { defaultValue: 'NPC' }),
      select('inverterModulationScheme', 'Modulation scheme', ['SPWM', 'SVM'], 'Inverter modulation method.', { defaultValue: 'SVM' }),
      number('inverterSwitchingFrequency', 'Inverter switching frequency', 'Hz', 'Inverter switching frequency.', { min: 0, defaultValue: 16000 }),
      number('CDC', 'DC-link capacitance', 'F', 'DC-link capacitance.', { min: 0, defaultValue: 0.010351 }),
      number('L1', 'Converter-side inductance L1', 'H', 'Converter-side filter inductance.', { min: 0, defaultValue: 0.0003 }),
      number('R1', 'Converter-side resistance R1', 'Ω', 'Converter-side filter resistance.', { min: 0, defaultValue: 0.03 }),
      number('C', 'Filter capacitance C', 'F', 'AC filter capacitance.', { min: 0, defaultValue: 0.00009786 }),
      number('L2', 'Grid-side inductance L2', 'H', 'Grid-side filter inductance.', { min: 0, defaultValue: 0.00093183 }),
      number('R2', 'Grid-side resistance R2', 'Ω', 'Grid-side filter resistance.', { min: 0, defaultValue: 0.0053 }),
      number('boostCapacitance', 'Boost capacitance', 'F', 'Boost-stage capacitance.', { min: 0, enabledWhen: { pvInverterConfiguration: 'true' }, defaultValue: 0.010351 }),
      number('boostInductance', 'Boost inductance', 'H', 'Boost-stage inductance.', { min: 0, enabledWhen: { pvInverterConfiguration: 'true' }, defaultValue: 0.0005 }),
      number('boostResistance', 'Boost resistance', 'Ω', 'Boost-inductor series resistance.', { min: 0, enabledWhen: { pvInverterConfiguration: 'true' }, defaultValue: 0.005 }),
      number('boostSwitchingFrequency', 'Boost switching frequency', 'Hz', 'Boost-converter switching frequency.', { min: 0, enabledWhen: { pvInverterConfiguration: 'true' }, defaultValue: 16000 }),
    ],
    outputs: [output('avgODE', 'Float[]', 'Section 2.1.4', 'Average-model ODE.'), output('swODE', 'Float[]', 'Section 2.1.3', 'Switching-model ODE.')],
  },
  control: {
    title: 'Control', section: '1.4',
    fields: [select('controlStrategy', 'Control strategy', ['Open loop', 'Closed loop – traditional', 'Closed loop – advanced'], 'Control strategy used to retrieve or generate duty ratios.', { defaultValue: 'Open loop' })],
    outputs: [output('da', 'Float[]', 'N/A', 'Phase-A duty ratio.'), output('db', 'Float[]', 'N/A', 'Phase-B duty ratio.'), output('dc', 'Float[]', 'N/A', 'Phase-C duty ratio.')],
  },
  dynamicEvents: {
    title: 'Dynamic Events', section: '1.5',
    fields: [
      number('latitude', 'Latitude', '°', 'Latitude used to query the event database.', { min: -90, max: 90, defaultValue: 39.9042 }),
      number('longitude', 'Longitude', '°', 'Longitude used to query the event database.', { min: -180, max: 180, defaultValue: 116.4074 }),
      number('eventID', 'Event ID', 'N/A', 'Dynamic-event identifier.', { min: 0, step: 1, defaultValue: 0 }),
    ],
    outputs: [output('eventType', 'Enum', 'N/A', 'Dynamic-event category.'), output('eventTime', 'Datetime', 's', 'Event timestamp.'), output('eventDuration', 'Float', 's', 'Event duration.')],
  },
  powerModule: { title: 'Power Module', section: '1.6.1', fields: [text('pmPartNumber', 'Power-module part number', 'N/A', 'Manufacturer part number.', { defaultValue: 'F3L75R12W1H3_B11' })], outputs: [output('rth', 'Float', '°C/W', 'Thermal resistance.'), output('Von_Ic_Tj', 'Float[]', 'V', 'On-state voltage versus collector current and temperature.'), output('Eon_Ic_Vce_Tj', 'Float[]', 'J', 'Turn-on energy-loss surface.'), output('Eoff_Ic_Vce_Tj', 'Float[]', 'J', 'Turn-off energy-loss surface.'), output('ERR_Ic_Vce_Tj', 'Float[]', 'J', 'Reverse-recovery energy-loss surface.'), output('PM_reliability_coef_thermal', 'Float[]', 'N/A', 'Thermal reliability coefficients.'), output('PM_reliability_coef_RH', 'Float[]', 'N/A', 'Humidity reliability coefficients.')] },
  capacitor: { title: 'Capacitor', section: '1.6.2', fields: [select('capType', 'Capacitor type', ['Film', 'Electrolytic'], 'Capacitor technology.', { defaultValue: 'Electrolytic' }), text('capPartNumber', 'Capacitor part number', 'N/A', 'Manufacturer part number.', { defaultValue: 'Rubycon_475VXG330MEFCSN30X55' })], outputs: [output('Capacitance_T_V', 'Float[]', 'F', 'Capacitance versus temperature and voltage.'), output('Esr_T_freq', 'Float[]', 'Ω', 'ESR versus temperature and frequency.'), output('Rth_ah', 'Float[]', '°C/W', 'Ambient-to-hotspot thermal resistance.'), output('Rth_as', 'Float[]', '°C/W', 'Ambient-to-surface thermal resistance.'), output('Cap_freq', 'Integer', 'Hz', 'DC-link capacitor frequency.'), output('Cap_reliability_coef', 'Float[]', 'N/A', 'Reliability coefficients.')] },
  pcb: { title: 'PCB', section: '1.6.3', fields: [select('footprint', 'Footprint', ['Decoupling', 'MELF'], 'PCB-mounted component footprint.', { defaultValue: 'MELF' }), select('solderJointMaterial', 'Solder-joint material', ['Leaded SnPb', 'Leadless SAC'], 'Solder material.', { defaultValue: 'Leadless SAC' }), number('thickness', 'PCB thickness', 'mm', 'PCB thickness.', { min: 0, defaultValue: 1.57 }), text('dimensions', 'Component/pad/joint dimensions', 'mm', 'Comma-separated physical dimensions.', { defaultValue: '5.9,2.2,2.0,1.7,2.4,0.035,1.5,2.4,0.06' })], outputs: [output('PCB_reliability_coef', 'Float[]', 'N/A', 'PCB reliability coefficients.')] },
  coolingFan: { title: 'Cooling Fan', section: '1.6.4', fields: [text('coolingFanPartNumber', 'Cooling-fan part number', 'N/A', 'Manufacturer part number.', { defaultValue: 'NMB 09238RE-24N-GU' })], outputs: [output('CoolingFan_reliability_coef_mechanical', 'Float[]', 'N/A', 'Mechanical-failure coefficients.'), output('CoolingFan_reliability_coef_electrical', 'Float[]', 'N/A', 'Electrical-failure coefficients.')] },
  mov: { title: 'MOV', section: '1.6.5', fields: [text('movPartNumber', 'MOV part number', 'N/A', 'Manufacturer part number.', { defaultValue: 'N/A' })], outputs: [output('ivCurve', 'Float[]', 'V/A', 'Current–voltage characteristic.'), output('pulseRatingCurve', 'Float[]', 'Stress/cycles', 'Electrical cycling lifetime curve.')] },
  relay: { title: 'Relay', section: '1.6.6', fields: [text('relayPartNumber', 'Relay part number', 'N/A', 'Manufacturer part number.', { defaultValue: 'N/A' })], outputs: [output('ivCurve', 'Float[]', 'V/A', 'Current–voltage characteristic.'), output('electricalLifeCurve', 'Float[]', 'Stress/cycles', 'Electrical lifetime curve.')] },
};

export function initialValues(schema) {
  return Object.fromEntries(schema.fields.map((field) => [field.name, field.defaultValue ?? (field.type === 'select' ? field.options[0] : '')]));
}

export function defaultWorkflowInputs() {
  return Object.fromEntries(Object.entries(workflowToolboxes).map(([key, schema]) => [key, initialValues(schema)]));
}

function isBlank(value) {
  return value === undefined || value === null || (typeof value === 'string' && value.trim() === '');
}

function mergeWithDefaults(defaults, stored = {}) {
  return Object.fromEntries(Object.entries(defaults).map(([key, values]) => {
    const storedValues = stored[key] || {};
    return [key, Object.fromEntries(Object.entries(values).map(([field, defaultValue]) => {
      const storedValue = storedValues[field];
      return [field, isBlank(storedValue) ? defaultValue : storedValue];
    }))];
  }));
}

function persistWorkflowInputs(inputs) {
  localStorage.setItem(WORKFLOW_STORAGE_KEY, JSON.stringify(inputs));
  return inputs;
}

export function readWorkflowInputs() {
  const defaults = defaultWorkflowInputs();
  try {
    const stored = JSON.parse(localStorage.getItem(WORKFLOW_STORAGE_KEY) || '{}');
    // Repair older browser data that contains empty required fields. This lets
    // the simulator run immediately with the project defaults while preserving
    // every non-empty user override.
    return persistWorkflowInputs(mergeWithDefaults(defaults, stored));
  } catch {
    return defaults;
  }
}

export function resetWorkflowInputsToDefaults() {
  const defaults = persistWorkflowInputs(defaultWorkflowInputs());
  window.dispatchEvent(new Event('tracepv-inputs-changed'));
  return defaults;
}

export function saveWorkflowInput(key, value) {
  const current = readWorkflowInputs();
  const next = { ...current, [key]: value };
  localStorage.setItem(WORKFLOW_STORAGE_KEY, JSON.stringify(next));
  window.dispatchEvent(new Event('tracepv-inputs-changed'));
  return next;
}
