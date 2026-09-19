import React, { useMemo, useState } from 'react';
import {
  initialValues,
  readWorkflowInputs,
  resetWorkflowInputsToDefaults,
  saveWorkflowInput,
  workflowToolboxes,
} from './workflowSchema';

const GROUPS = [
  ['missionProfile', 'Mission Profile'],
  ['pvSystem', 'PV System'],
  ['gridTransformer', 'Grid and Transformer'],
  ['pvInverter', 'PV Inverter'],
  ['control', 'Control'],
  ['dynamicEvents', 'Dynamic Events'],
  ['powerModule', 'Power Module'],
  ['capacitor', 'Capacitor'],
  ['pcb', 'PCB'],
  ['coolingFan', 'Cooling Fan'],
  ['mov', 'MOV'],
  ['relay', 'Relay'],
];

function buildInitialCollection() {
  const stored = readWorkflowInputs();
  return Object.fromEntries(GROUPS.map(([key]) => [key, {
    ...initialValues(workflowToolboxes[key]),
    ...(stored[key] || {}),
  }]));
}

export default function WorkflowInputCollection({ compact = false }) {
  const initial = useMemo(buildInitialCollection, []);
  const [collection, setCollection] = useState(initial);
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState('');

  function enabled(field, values) {
    if (!field.enabledWhen) return true;
    return Object.entries(field.enabledWhen).every(([key, value]) => values[key] === value);
  }

  function update(groupKey, fieldName, value) {
    setSaved(false);
    setCollection((current) => ({
      ...current,
      [groupKey]: { ...current[groupKey], [fieldName]: value },
    }));
  }

  function useDefaults() {
    const defaults = resetWorkflowInputsToDefaults();
    setCollection(defaults);
    setError('');
    setSaved(true);
  }

  function submit(event) {
    event.preventDefault();
    const missing = [];
    GROUPS.forEach(([groupKey, title]) => {
      const schema = workflowToolboxes[groupKey];
      schema.fields.forEach((field) => {
        if (field.required && enabled(field, collection[groupKey]) && collection[groupKey][field.name] === '') missing.push(`${title}: ${field.label}`);
      });
    });
    if (missing.length) {
      setSaved(false);
      setError(`Complete required inputs: ${missing.join(', ')}`);
      return;
    }
    setError('');
    GROUPS.forEach(([groupKey]) => {
      const schema = workflowToolboxes[groupKey];
      const normalized = Object.fromEntries(schema.fields.map((field) => [
        field.name,
        field.type === 'number' && collection[groupKey][field.name] !== ''
          ? Number(collection[groupKey][field.name])
          : collection[groupKey][field.name],
      ]));
      saveWorkflowInput(groupKey, normalized);
    });
    setSaved(true);
  }

  return (
    <form className={`input-collection ${compact ? 'compact' : ''}`} onSubmit={submit}>
      <div className="section-title">
        <div><h2>Parameter Collection</h2><p>Complete simulator inputs in one place.</p></div>
        <button type="button" className="secondary" onClick={useDefaults}>Use default settings</button>
        <button type="submit">Save all inputs</button>
      </div>
      <div className="input-groups">
        {GROUPS.map(([groupKey, title], index) => {
          const schema = workflowToolboxes[groupKey];
          const values = collection[groupKey];
          return (
            <details key={groupKey} open={!compact && index < 2}>
              <summary>{title}<span>{schema.fields.length} inputs</span></summary>
              <div className="collection-field-grid">
                {schema.fields.map((field) => {
                  const active = enabled(field, values);
                  return (
                    <label key={`${groupKey}-${field.name}`} className={!active ? 'disabled-field' : ''}>
                      <span>{field.label} {field.unit !== 'N/A' && <em>{field.unit}</em>}</span>
                      {field.type === 'select' ? (
                        <select value={values[field.name]} disabled={!active} onChange={(event) => update(groupKey, field.name, event.target.value)}>
                          {field.options.map((option) => <option key={option} value={option}>{option}</option>)}
                        </select>
                      ) : (
                        <input type={field.type} value={values[field.name]} disabled={!active} min={field.min} max={field.max} step={field.step || 'any'} onChange={(event) => update(groupKey, field.name, event.target.value)} />
                      )}
                      {!compact && <small>{field.description}</small>}
                    </label>
                  );
                })}
              </div>
            </details>
          );
        })}
      </div>
      {error && <div className="collection-error">{error}</div>}
      {saved && <div className="collection-saved">All simulator inputs were saved.</div>}
    </form>
  );
}
