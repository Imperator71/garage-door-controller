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

const MQTT_TOPICS = ['garage/door/state', 'garage/pir', 'garage/radar'];
const MAX_EVENTS  = 500;

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
  mqttClient.subscribe(MQTT_TOPICS, (err) => {
    if (err) console.error('[mqtt] subscribe error:', err.message);
  });
});

mqttClient.on('message', (topic, message) => {
  const value = message.toString();
  console.log(`[mqtt] ${topic}: ${value}`);
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

// DELETE /templates/:id
app.delete('/templates/:id', (req, res) => {
  const key = req.params.id;
  if (!db.templates[key]) return res.status(404).json({ error: 'Not found' });
  delete db.templates[key];
  saveData(db);
  res.json({ deleted: parseInt(key, 10) });
});

// 404 catch-all
app.use((req, res) => res.status(404).json({ error: 'Not found' }));

app.listen(PORT, () => console.log(`[http] fingerprint registry listening on :${PORT}`));
