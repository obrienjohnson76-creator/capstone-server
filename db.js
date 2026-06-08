const admin = require('firebase-admin');
const fs = require('fs');
const path = require('path');

const demoFile = path.join(__dirname, 'database.local.json');
let firestore = null;
let localMode = process.env.LOCAL_DEMO_MODE === 'true';

function nowIso() {
  return new Date().toISOString();
}

function loadLocal() {
  if (!fs.existsSync(demoFile)) {
    fs.writeFileSync(demoFile, JSON.stringify({ users: [], pendingUsers: [], readings: [], latest: null }, null, 2));
  }
  return JSON.parse(fs.readFileSync(demoFile, 'utf8'));
}

function saveLocal(data) {
  fs.writeFileSync(demoFile, JSON.stringify(data, null, 2));
}

function initDatabase() {
  if (localMode) {
    console.log('LOCAL_DEMO_MODE=true. Using database.local.json instead of Firebase.');
    return;
  }

  const keyPath = path.resolve(__dirname, process.env.FIREBASE_SERVICE_ACCOUNT || './serviceAccountKey.json');
  if (!fs.existsSync(keyPath)) {
    console.warn('\nFirebase service account not found.');
    console.warn(`Expected: ${keyPath}`);
    console.warn('The server will run in LOCAL DEMO MODE until serviceAccountKey.json is added.\n');
    localMode = true;
    return;
  }

  const serviceAccount = require(keyPath);
  admin.initializeApp({ credential: admin.credential.cert(serviceAccount) });
  firestore = admin.firestore();
  console.log('Connected to Firebase Firestore.');
}

function serverTimestamp() {
  return firestore ? admin.firestore.FieldValue.serverTimestamp() : nowIso();
}

async function findOne(collection, field, value) {
  if (localMode) {
    const data = loadLocal();
    return data[collection].find(item => item[field] === value) || null;
  }
  const snapshot = await firestore.collection(collection).where(field, '==', value).limit(1).get();
  if (snapshot.empty) return null;
  const doc = snapshot.docs[0];
  return { id: doc.id, ...doc.data() };
}

async function addDoc(collection, payload) {
  if (localMode) {
    const data = loadLocal();
    const doc = { id: `${Date.now()}-${Math.random().toString(16).slice(2)}`, ...payload };
    data[collection].push(doc);
    saveLocal(data);
    return doc;
  }
  const ref = await firestore.collection(collection).add(payload);
  return { id: ref.id, ...payload };
}

async function setDoc(collection, id, payload) {
  if (localMode) {
    const data = loadLocal();
    if (collection === 'latest') {
      data.latest = { id, ...payload };
    } else {
      const index = data[collection].findIndex(item => item.id === id);
      if (index >= 0) data[collection][index] = { ...data[collection][index], ...payload };
      else data[collection].push({ id, ...payload });
    }
    saveLocal(data);
    return { id, ...payload };
  }
  await firestore.collection(collection).doc(id).set(payload, { merge: true });
  return { id, ...payload };
}

async function deleteDoc(collection, id) {
  if (localMode) {
    const data = loadLocal();
    data[collection] = data[collection].filter(item => item.id !== id);
    saveLocal(data);
    return;
  }
  await firestore.collection(collection).doc(id).delete();
}

async function getDoc(collection, id) {
  if (localMode) {
    const data = loadLocal();
    if (collection === 'latest') return data.latest;
    return data[collection].find(item => item.id === id) || null;
  }
  const snap = await firestore.collection(collection).doc(id).get();
  return snap.exists ? { id: snap.id, ...snap.data() } : null;
}

async function listDocs(collection, options = {}) {
  if (localMode) {
    const data = loadLocal();
    let items = [...(data[collection] || [])];
    if (options.orderBy) {
      const [field, direction] = options.orderBy;
      items.sort((a, b) => String(a[field] || '').localeCompare(String(b[field] || '')));
      if (direction === 'desc') items.reverse();
    }
    if (options.limit) items = items.slice(0, options.limit);
    return items;
  }

  let query = firestore.collection(collection);
  if (options.orderBy) query = query.orderBy(options.orderBy[0], options.orderBy[1]);
  if (options.limit) query = query.limit(options.limit);
  const snapshot = await query.get();
  return snapshot.docs.map(doc => ({ id: doc.id, ...doc.data() }));
}

module.exports = {
  initDatabase,
  serverTimestamp,
  findOne,
  addDoc,
  setDoc,
  deleteDoc,
  getDoc,
  listDocs,
  isLocalMode: () => localMode
};
