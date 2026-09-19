import React, { useMemo, useState } from 'react';
import { initialValues, readWorkflowInputs, saveWorkflowInput, workflowToolboxes } from './workflowSchema';

export default function WorkflowToolboxForm({ toolboxKey }) {
  const schema = workflowToolboxes[toolboxKey];
  const stored = useMemo(() => readWorkflowInputs()[toolboxKey], [toolboxKey]);
  const [values, setValues] = useState({ ...initialValues(schema), ...(stored || {}) });
  const [saved, setSaved] = useState(false);

  function isEnabled(field) {
    if (!field.enabledWhen) return true;
    return Object.entries(field.enabledWhen).every(([key, value]) => values[key] === value);
  }

  function submit(event) {
    event.preventDefault();
    const normalized = Object.fromEntries(schema.fields.map((field) => [
      field.name,
      field.type === 'number' && values[field.name] !== '' ? Number(values[field.name]) : values[field.name],
    ]));
    saveWorkflowInput(toolboxKey, normalized);
    setValues(normalized);
    setSaved(true);
  }

  function download() {
    const blob = new Blob([JSON.stringify({ [toolboxKey]: values }, null, 2)], { type: 'application/json' });
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob); link.download = `${toolboxKey}.json`; link.click();
    URL.revokeObjectURL(link.href);
  }

  return (
    <main className="workflow-form-page">
      <form className="workflow-form" onSubmit={submit}>
        <header><div><span>Workflow section {schema.section}</span><h1>{schema.title}</h1></div><p>Fields follow the workflow document input contract.</p></header>
        <section className="workflow-field-grid">
          {schema.fields.map((field) => {
            const enabled = isEnabled(field);
            return (
              <label key={field.name} className={!enabled ? 'disabled-field' : ''}>
                <span>{field.label} <code>{field.name}</code></span>
                {field.type === 'select' ? (
                  <select value={values[field.name]} disabled={!enabled} onChange={(e) => setValues({ ...values, [field.name]: e.target.value })}>
                    {field.options.map((option) => <option key={option} value={option}>{option}</option>)}
                  </select>
                ) : (
                  <input type={field.type} value={values[field.name]} disabled={!enabled} required={enabled && field.required} min={field.min} max={field.max} step={field.step || 'any'} onChange={(e) => setValues({ ...values, [field.name]: e.target.value })} />
                )}
                <small><b>{field.unit}</b> · {field.description}</small>
              </label>
            );
          })}
        </section>
        {schema.outputs.length > 0 && <section className="workflow-outputs"><h2>Toolbox outputs (read only)</h2><div>{schema.outputs.map((item) => <article key={item.name}><code>{item.name}</code><span>{item.type} · {item.unit}</span><p>{item.description}</p></article>)}</div></section>}
        <footer><button type="submit">Save to Input Collection</button><button type="button" className="secondary" onClick={download}>Download JSON</button>{saved && <strong>Saved</strong>}</footer>
      </form>
    </main>
  );
}
