require('dotenv').config();
const path = require('path');
const express = require('express');
const session = require('express-session');
const bcrypt = require('bcryptjs');
const helmet = require('helmet');
const cors = require('cors');
const db = require('./db');

const app = express();
const PORT = Number(process.env.PORT || 5000);
const MIN_ESP_UPDATE_SECONDS = Number(process.env.MIN_ESP_UPDATE_SECONDS || 10);
let lastAcceptedEspAt = 0;

db.initDatabase();

app.use(helmet({ contentSecurityPolicy: false }));
app.use(cors({ origin: true, credentials: true }));
app.use(express.json({ limit: '1mb' }));
app.use(express.urlencoded({ extended: true }));
app.use(session({
  secret: process.env.SESSION_SECRET || 'change-this-session-secret',
  resave: false,
  saveUninitialized: false,
  cookie: { httpOnly: true, sameSite: 'lax', maxAge: 1000 * 60 * 60 * 8 }
}));
app.use(express.static(path.join(__dirname, 'public')));


function normalizeUsername(username) {
  return String(username || '').trim().toLowerCase();
}

function publicUser(user) {
  if (!user) return null;
  return {
    id: user.id,
    username: user.username,
    name: user.name,
    position: user.position,
    approved: user.approved,
    isAdmin: Boolean(user.isAdmin)
  };
}

async function seedAdmin() {
  const username = normalizeUsername(process.env.ADMIN_USERNAME || 'admin');
  const existing = await db.findOne('users', 'username', username);
  if (existing) return;

  const passwordHash = await bcrypt.hash(process.env.ADMIN_PASSWORD || 'Admin12345!', 12);
  await db.addDoc('users', {
    username,
    passwordHash,
    name: process.env.ADMIN_NAME || 'System Administrator',
    position: process.env.ADMIN_POSITION || 'Lab Tech',
    approved: true,
    isAdmin: true,
    createdAt: db.serverTimestamp(),
    createdAtIso: new Date().toISOString()
  });
  console.log(`Default admin created. Username: ${username}`);
}

function requireAuth(req, res, next) {
  if (!req.session.user) return res.status(401).json({ error: 'Not logged in' });
  next();
}

function requireAdmin(req, res, next) {
  if (!req.session.user?.isAdmin) return res.status(403).json({ error: 'Admin approval access required' });
  next();
}

function safeNumber(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function normalizeReading(body) {
  const voltage = safeNumber(body.voltage);
  const current = safeNumber(body.current);
  const powerFactor = safeNumber(body.powerFactor ?? body.pf, 1);
  const power = safeNumber(body.power, voltage * current * powerFactor);
  const energy = safeNumber(body.energy ?? body.energyKwh ?? body.kwh);

  const phaseA = body.phaseA || body.a || {};
  const phaseB = body.phaseB || body.b || {};
  const phaseC = body.phaseC || body.c || {};

  return {
    voltage,
    current,
    power,
    energy,
    powerFactor,
    frequency: safeNumber(body.frequency, 50),
    phaseA: { voltage: safeNumber(phaseA.voltage, voltage), current: safeNumber(phaseA.current, current) },
    phaseB: { voltage: safeNumber(phaseB.voltage, voltage), current: safeNumber(phaseB.current, current) },
    phaseC: { voltage: safeNumber(phaseC.voltage, voltage), current: safeNumber(phaseC.current, current) },
    source: body.source || 'ESP32',
    timestamp: db.serverTimestamp(),
    timestampIso: new Date().toISOString()
  };
}

app.get('/api/health', (req, res) => {
  res.json({ ok: true, firebaseMode: !db.isLocalMode(), minEspUpdateSeconds: MIN_ESP_UPDATE_SECONDS });
});

app.get('/api/session', (req, res) => {
  res.json({ user: req.session.user || null, firebaseMode: !db.isLocalMode() });
});

app.post('/api/register', async (req, res) => {
  try {
    const username = normalizeUsername(req.body.username);
    const password = String(req.body.password || '');
    const confirmPassword = String(req.body.confirmPassword || '');
    const name = String(req.body.name || '').trim();
    const position = String(req.body.position || '').trim();
    const allowed = ['Dean', 'Lecturer', 'Lab Tech', 'Student'];

    if (!username || !password || !confirmPassword || !name || !allowed.includes(position)) {
      return res.status(400).json({ error: 'Complete all fields and choose a valid position.' });
    }
    if (password !== confirmPassword) return res.status(400).json({ error: 'Passwords do not match.' });
    if (password.length < 8) return res.status(400).json({ error: 'Password must be at least 8 characters.' });
    if (await db.findOne('users', 'username', username) || await db.findOne('pendingUsers', 'username', username)) {
      return res.status(409).json({ error: 'That username already exists or is awaiting approval.' });
    }

    const passwordHash = await bcrypt.hash(password, 12);
    await db.addDoc('pendingUsers', { username, passwordHash, name, position, createdAt: db.serverTimestamp(), createdAtIso: new Date().toISOString() });
    res.json({ message: 'Account request sent. An admin must approve it before login works.' });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: 'Could not create request.' });
  }
});

app.post('/api/login', async (req, res) => {
  try {
    const username = normalizeUsername(req.body.username);
    const password = String(req.body.password || '');
    const user = await db.findOne('users', 'username', username);
    if (!user || !user.approved) return res.status(401).json({ error: 'Invalid username, password, or account not approved.' });
    const ok = await bcrypt.compare(password, user.passwordHash || '');
    if (!ok) return res.status(401).json({ error: 'Invalid username, password, or account not approved.' });
    req.session.user = publicUser(user);
    res.json({ user: req.session.user });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: 'Could not log in.' });
  }
});

app.post('/api/logout', (req, res) => {
  req.session.destroy(() => res.json({ ok: true }));
});

app.post('/api/esp/data', async (req, res) => {
  try {
    const apiKey = req.headers['x-api-key'] || req.body.apiKey;
    if (apiKey !== process.env.ESP_API_KEY) return res.status(401).json({ error: 'Invalid ESP API key.' });

    const now = Date.now();
    const waitMs = MIN_ESP_UPDATE_SECONDS * 1000 - (now - lastAcceptedEspAt);
    if (waitMs > 0) return res.status(429).json({ error: `Sending too fast. Try again in ${Math.ceil(waitMs / 1000)} seconds.` });
    lastAcceptedEspAt = now;

    const reading = normalizeReading(req.body);
    await db.addDoc('readings', reading);
    await db.setDoc('latest', 'reading', reading);
    res.json({ ok: true, acceptedAt: reading.timestampIso, minUpdateSeconds: MIN_ESP_UPDATE_SECONDS });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: 'Could not save ESP reading.' });
  }
});

app.get('/api/latest', requireAuth, async (req, res) => {
  const latest = await db.getDoc('latest', 'reading');
  res.json(latest || null);
});

app.get('/api/readings', requireAuth, async (req, res) => {
  const limit = Math.min(Number(req.query.limit || 100), 500);
  const readings = await db.listDocs('readings', { orderBy: ['timestampIso', 'desc'], limit });
  res.json(readings);
});

app.get('/api/pending-users', requireAuth, requireAdmin, async (req, res) => {
  const users = await db.listDocs('pendingUsers', { orderBy: ['createdAtIso', 'desc'], limit: 100 });
  res.json(users.map(({ passwordHash, ...u }) => u));
});

app.post('/api/users/:id/approve', requireAuth, requireAdmin, async (req, res) => {
  const pending = await db.getDoc('pendingUsers', req.params.id);
  if (!pending) return res.status(404).json({ error: 'Pending user not found.' });
  const { id, ...userData } = pending;
  await db.addDoc('users', { ...userData, approved: true, isAdmin: false, approvedAt: db.serverTimestamp(), approvedAtIso: new Date().toISOString() });
  await db.deleteDoc('pendingUsers', req.params.id);
  res.json({ ok: true });
});

app.post('/api/users/:id/reject', requireAuth, requireAdmin, async (req, res) => {
  await db.deleteDoc('pendingUsers', req.params.id);
  res.json({ ok: true });
});

app.get('*', (req, res) => res.sendFile(path.join(__dirname, 'public', 'index.html')));

seedAdmin().then(() => {
  app.listen(PORT, "0.0.0.0", () => {
    console.log(`monitor-new running at http://localhost:${PORT}`);
    console.log(`ESP POST endpoint: http://0.0.0.0:${PORT}/api/esp/data`);
  });
}).catch(err => {
  console.error('Startup failed:', err);
  process.exit(1);
});
