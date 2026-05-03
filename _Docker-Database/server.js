'use strict';

const express = require('express');
const mqtt    = require('mqtt');
const fs      = require('fs');
const path    = require('path');

// ── Config ────────────────────────────────────────────────────
const PORT      = parseInt(process.env.PORT      || '8080', 10);
const MQTT_URL  = process.env.MQTT_URL            || 'mqtt://192.168.1.1';
const MQTT_USER = process.env.MQTT_USER           || '';
const MQTT_PASS = process.env.MQTT_PASS           || '';
const DATA_FILE = process.env.DATA_FILE           || '/data/templates.json';

const MQTT_TOPICS = ['garage/door/state', 'garage/pir', 'garage/radar', 'garage/fingerprint'];
const FP_CMD_TOPIC    = 'garage/fingerprint/cmd';
const FP_RESULT_TOPIC = 'garage/fingerprint/result';
const FP_SLOTS_TOPIC  = 'garage/fingerprint/slots';
const MAX_EVENTS  = 500;

// Latest fingerprint result (for polling)
let lastFpResult = null;   // { value, ts }
let sensorSlots  = [];     // slot IDs currently on the sensor

// ── Persistence ───────────────────────────────────────────────
function loadData() {
  try {
    return JSON.parse(fs.readFileSync(DATA_FILE, 'utf8'));
  } catch {
    return { templates: {}, events: [] };
  }
}

function saveData(data) {
  const dir = path.dirname(DATA_FILE);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(DATA_FILE, JSON.stringify(data, null, 2));
}

const db = loadData();

// ── MQTT client ───────────────────────────────────────────────
const mqttOpts = {};
if (MQTT_USER) mqttOpts.username = MQTT_USER;
if (MQTT_PASS) mqttOpts.password = MQTT_PASS;

const mqttClient = mqtt.connect(MQTT_URL, mqttOpts);

mqttClient.on('connect', () => {
  console.log(`[mqtt] connected → ${MQTT_URL}`);
  mqttClient.subscribe([...MQTT_TOPICS, FP_RESULT_TOPIC, FP_SLOTS_TOPIC], (err) => {
    if (err) console.error('[mqtt] subscribe error:', err.message);
  });
});

mqttClient.on('message', (topic, message) => {
  const value = message.toString();
  console.log(`[mqtt] ${topic}: ${value}`);

  if (topic === FP_RESULT_TOPIC) {
    lastFpResult = { value, ts: new Date().toISOString() };
    return;
  }
  if (topic === FP_SLOTS_TOPIC) {
    sensorSlots = value ? value.split(',').map(Number) : [];
    console.log(`[mqtt] sensor slots: [${sensorSlots}]`);
    return;
  }

  db.events.push({ topic, value, ts: new Date().toISOString() });
  if (db.events.length > MAX_EVENTS) db.events.splice(0, db.events.length - MAX_EVENTS);
  saveData(db);
});

mqttClient.on('error',   (err) => console.error('[mqtt] error:', err.message));
mqttClient.on('offline', ()    => console.warn('[mqtt] broker unreachable, retrying…'));

// ── HTTP API ──────────────────────────────────────────────────
const app = express();
app.use(express.json());

// GET /status  — latest retained value for each garage topic
app.get('/status', (req, res) => {
  const status = {};
  for (const topic of MQTT_TOPICS) {
    const last = [...db.events].reverse().find(e => e.topic === topic);
    if (last) status[topic] = { value: last.value, ts: last.ts };
  }
  res.json(status);
});

// GET /events?limit=50  — recent MQTT events (newest first)
app.get('/events', (req, res) => {
  const limit = Math.min(parseInt(req.query.limit || '50', 10), MAX_EVENTS);
  res.json(db.events.slice(-limit).reverse());
});

// GET /templates  — list all registered fingerprints
app.get('/templates', (req, res) => {
  const list = Object.entries(db.templates).map(([id, info]) => ({
    id: parseInt(id, 10),
    ...info,
  }));
  list.sort((a, b) => a.id - b.id);
  res.json(list);
});

// GET /templates/:id
app.get('/templates/:id', (req, res) => {
  const entry = db.templates[req.params.id];
  if (!entry) return res.status(404).json({ error: 'Not found' });
  res.json({ id: parseInt(req.params.id, 10), ...entry });
});

// POST /templates  — register or update a fingerprint slot
// Body: { id: <number>, name: <string>, [note: <string>] }
app.post('/templates', (req, res) => {
  const { id, name, ...extra } = req.body;
  if (typeof id !== 'number' || !Number.isInteger(id) || id < 0) {
    return res.status(400).json({ error: '"id" must be a non-negative integer' });
  }
  if (typeof name !== 'string' || !name.trim()) {
    return res.status(400).json({ error: '"name" must be a non-empty string' });
  }
  db.templates[id] = { name: name.trim(), ...extra, updatedAt: new Date().toISOString() };
  saveData(db);
  res.status(201).json({ id, ...db.templates[id] });
});

// DELETE /templates/:id  — also sends sensor delete command via MQTT
app.delete('/templates/:id', (req, res) => {
  const key = req.params.id;
  if (!db.templates[key]) return res.status(404).json({ error: 'Not found' });
  delete db.templates[key];
  saveData(db);
  mqttClient.publish(FP_CMD_TOPIC, `DELETE:${key}`);
  res.json({ deleted: parseInt(key, 10) });
});

// POST /fingerprint/enroll  — trigger sensor enrollment, returns 202 immediately
// Body: { id: <number> }   Poll GET /fingerprint/result for outcome.
app.post('/fingerprint/enroll', (req, res) => {
  const { id } = req.body;
  if (typeof id !== 'number' || !Number.isInteger(id) || id < 0) {
    return res.status(400).json({ error: '"id" must be a non-negative integer' });
  }
  lastFpResult = null;  // clear stale result
  mqttClient.publish(FP_CMD_TOPIC, `ENROLL:${id}`);
  res.status(202).json({ status: 'enrolling', id });
});

// GET /fingerprint/result  — latest result from sensor (poll after enroll)
app.get('/fingerprint/result', (req, res) => {
  res.json(lastFpResult || { value: null, ts: null });
});

// GET /fingerprint/slots  — slot IDs currently enrolled on the physical sensor
app.get('/fingerprint/slots', (req, res) => {
  res.json({ slots: sensorSlots });
});

// ── UI ────────────────────────────────────────────────────────
app.get('/', (req, res) => {
  res.send(`<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Garage Monitor</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#111;color:#e0e0e0;padding:1.5rem}
  h1{font-size:1.3rem;margin-bottom:1.2rem;color:#fff;letter-spacing:.05em}
  h2{font-size:.85rem;text-transform:uppercase;letter-spacing:.1em;color:#888;margin-bottom:.6rem}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:.8rem;margin-bottom:1.5rem}
  .card{background:#1e1e1e;border-radius:10px;padding:1rem;text-align:center}
  .card .label{font-size:.7rem;color:#888;margin-bottom:.4rem;text-transform:uppercase}
  .card .value{font-size:1.5rem;font-weight:700}
  .OPEN,.ON{color:#4caf50}
  .CLOSED{color:#2196f3}
  .OFF{color:#555}
  .UNKNOWN{color:#ff9800}
  .DOWN,.DENIED,.ENROLL_MISMATCH,.ENROLL_TIMEOUT,.ENROLL_FAIL,.SENSOR_UNAVAILABLE{color:#f44336}
  .GRANTED,.ok-state{color:#4caf50}
  .enrolling-state{color:#ff9800}
  section{background:#1e1e1e;border-radius:10px;padding:1rem;margin-bottom:1.2rem}
  table{width:100%;border-collapse:collapse;font-size:.85rem}
  th{text-align:left;color:#888;font-weight:400;padding:.3rem .5rem;border-bottom:1px solid #333}
  td{padding:.35rem .5rem;border-bottom:1px solid #222;vertical-align:top}
  input{background:#2a2a2a;border:1px solid #444;color:#e0e0e0;border-radius:6px;padding:.4rem .6rem;font-size:.85rem;width:100%}
  button{background:#2979ff;border:none;color:#fff;border-radius:6px;padding:.4rem .8rem;cursor:pointer;font-size:.8rem}
  button.del{background:#c62828}
  button:hover{opacity:.85}
  .form-row{display:flex;gap:.5rem;margin-top:.8rem}
  .ts{color:#555;font-size:.75rem}
  #mqtt-status{font-size:.75rem;color:#888;margin-bottom:1rem}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}
  .dot.ok{background:#4caf50} .dot.err{background:#f44336}
</style>
</head>
<body>
<h1>🚗 Garage Monitor</h1>
<div id="mqtt-status"><span class="dot" id="mdot"></span><span id="mtext">connecting…</span></div>

<div class="grid" id="status-grid">
  <div class="card"><div class="label">Door</div><div class="value" id="s-door">…</div></div>
  <div class="card"><div class="label">PIR Motion</div><div class="value" id="s-pir">…</div></div>
  <div class="card"><div class="label">Radar</div><div class="value" id="s-radar">…</div></div>
  <div class="card"><div class="label">Fingerprint</div><div class="value" id="s-fp" style="font-size:1rem">…</div></div>
</div>

<section>
  <h2>Fingerprint Templates</h2>
  <table>
    <thead><tr><th>Slot</th><th>Name</th><th>Note</th><th>On Sensor</th><th>Updated</th><th></th></tr></thead>
    <tbody id="tpl-body"><tr><td colspan="6" style="color:#555">Loading…</td></tr></tbody>
  </table>
  <div class="form-row" style="margin-top:1rem;flex-wrap:wrap;gap:.5rem">
    <input id="tpl-id"   type="number" placeholder="Slot #" style="width:80px">
    <input id="tpl-name" type="text"   placeholder="Name">
    <input id="tpl-note" type="text"   placeholder="Note (optional)">
    <button onclick="saveMetaOnly()">Save Name Only</button>
    <button onclick="enrollOnSensor()" style="background:#e65100">Enroll on Sensor</button>
  </div>
  <div id="enroll-status" style="margin-top:.6rem;font-size:.85rem;min-height:1.2em"></div>
</section>

<section>
  <h2>Recent Events</h2>
  <table>
    <thead><tr><th>Time</th><th>Topic</th><th>Value</th></tr></thead>
    <tbody id="evt-body"><tr><td colspan="3" style="color:#555">Loading…</td></tr></tbody>
  </table>
</section>

<script>
async function refresh() {
  try {
    const [s, t, e] = await Promise.all([
      fetch('/status').then(r=>r.json()),
      fetch('/templates').then(r=>r.json()),
      fetch('/events?limit=30').then(r=>r.json()),
    ]);

    // Status cards
    const set = (id, val) => {
      const el = document.getElementById(id);
      el.textContent = val || '—';
      el.className = 'value ' + (val || '');
    };
    set('s-door',  s['garage/door/state']?.value);
    set('s-pir',   s['garage/pir']?.value);
    set('s-radar', s['garage/radar']?.value);
    const fpVal = s['garage/fingerprint']?.value || '';
    const fpEl = document.getElementById('s-fp');
    fpEl.textContent = fpVal || '—';
    fpEl.className = 'value ' + (fpVal.startsWith('GRANTED') ? 'ON' : fpVal === 'DENIED' ? 'DOWN' : fpVal === 'UNAVAILABLE' ? 'UNKNOWN' : '');

    // MQTT indicator — if we got any status data broker is reachable
    const connected = Object.keys(s).length > 0 || e.length > 0;
    document.getElementById('mdot').className  = 'dot ' + (connected ? 'ok' : 'err');
    document.getElementById('mtext').textContent = connected ? 'MQTT broker connected' : 'MQTT broker unreachable';

    // Templates
    const [slotRes] = await Promise.all([fetch('/fingerprint/slots').then(r=>r.json())]);
    const onSensor = new Set(slotRes.slots || []);
    const tb = document.getElementById('tpl-body');
    tb.innerHTML = t.length ? t.map(r =>
      '<tr><td>'+r.id+'</td><td>'+esc(r.name)+'</td><td>'+esc(r.note||'')+'</td>' +
      '<td style="text-align:center">'+(onSensor.has(r.id)?'<span class="ON">✓</span>':'<span class="DOWN">✗</span>')+'</td>' +
      '<td class="ts">'+(r.updatedAt||'').replace('T',' ').slice(0,16)+'</td>' +
      '<td><button class="del" onclick="delTemplate('+r.id+')">✕</button></td></tr>'
    ).join('') : '<tr><td colspan="6" style="color:#555">No templates registered</td></tr>';

    // Events
    const eb = document.getElementById('evt-body');
    eb.innerHTML = e.length ? e.map(ev =>
      '<tr><td class="ts">'+ev.ts.replace('T',' ').slice(0,19)+'</td>' +
      '<td>'+esc(ev.topic)+'</td><td class="'+ev.value+'">'+esc(ev.value)+'</td></tr>'
    ).join('') : '<tr><td colspan="3" style="color:#555">No events yet</td></tr>';
  } catch(err) {
    document.getElementById('mtext').textContent = 'Server unreachable';
    document.getElementById('mdot').className = 'dot err';
  }
}

function esc(s){ const d=document.createElement('div'); d.textContent=s; return d.innerHTML; }

function getFormValues() {
  const id   = parseInt(document.getElementById('tpl-id').value, 10);
  const name = document.getElementById('tpl-name').value.trim();
  const note = document.getElementById('tpl-note').value.trim();
  if (isNaN(id) || !name) { alert('Slot # and Name are required'); return null; }
  return { id, name, note };
}

function clearForm() {
  document.getElementById('tpl-id').value = '';
  document.getElementById('tpl-name').value = '';
  document.getElementById('tpl-note').value = '';
}

async function saveMetaOnly() {
  const v = getFormValues(); if (!v) return;
  await fetch('/templates', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ id: v.id, name: v.name, ...(v.note && {note: v.note}) })
  });
  clearForm();
  refresh();
}

let enrollPollTimer = null;

async function enrollOnSensor() {
  const v = getFormValues(); if (!v) return;
  // Save metadata first
  await fetch('/templates', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ id: v.id, name: v.name, ...(v.note && {note: v.note}) })
  });
  // Trigger enrollment
  const r = await fetch('/fingerprint/enroll', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ id: v.id })
  });
  if (!r.ok) { alert('Failed to send enroll command'); return; }
  clearForm();
  setEnrollStatus('enrolling-state', '⏳ Place finger on sensor… (30s timeout)');
  if (enrollPollTimer) clearInterval(enrollPollTimer);
  const started = Date.now();
  enrollPollTimer = setInterval(async () => {
    if (Date.now() - started > 35000) {
      clearInterval(enrollPollTimer);
      setEnrollStatus('DOWN', '⚠ No response from sensor');
      return;
    }
    const res = await fetch('/fingerprint/result').then(r=>r.json());
    if (!res.value) return;
    clearInterval(enrollPollTimer);
    if (res.value.startsWith('OK:')) {
      setEnrollStatus('ok-state', '✓ Enrolled successfully (slot '+res.value.split(':')[1]+')');
    } else if (res.value === 'ENROLL_MISMATCH') {
      setEnrollStatus('DOWN', '✗ Mismatch — fingers didn\'t match. Try again.');
    } else if (res.value === 'ENROLL_TIMEOUT') {
      setEnrollStatus('DOWN', '✗ Timed out — no finger detected');
    } else {
      setEnrollStatus('DOWN', '✗ '+res.value);
    }
    refresh();
  }, 1500);
}

function setEnrollStatus(cls, msg) {
  const el = document.getElementById('enroll-status');
  el.className = cls; el.textContent = msg;
}

async function addTemplate() { await saveMetaOnly(); }

async function delTemplate(id) {
  if (!confirm('Delete slot ' + id + ' from database AND sensor?')) return;
  await fetch('/templates/' + id, { method:'DELETE' });
  refresh();
}

refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>`);
});

// 404 catch-all (must be after all routes)
app.use((req, res) => res.status(404).json({ error: 'Not found' }));

app.listen(PORT, () => console.log(`[http] fingerprint registry listening on :${PORT}`));
