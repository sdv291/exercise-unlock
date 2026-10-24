'use strict';
// Exercise Unlock relay.
//
// The PC (ExerciseUnlockService, only while the lock screen is up) keeps a
// WebSocket open to /ws/pc and offers a pairing token. The lock screen shows
// it as a QR code: https://<host>/s/<TOKEN>. A phone that opens that page
// connects to /ws/phone?token=<TOKEN>; from then on the relay forwards small
// JSON messages both ways (exercise settings + status from the PC, "+1 squat"
// from the phone).
//
// Nothing is written to disk. PCs, tokens and phones live in memory and
// disappear with their connections.

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer, WebSocket } = require('ws');

const PORT = Number(process.env.PORT) || 8080;
const MAX_MSG_BYTES = 8 * 1024;        // real messages are well under 1 KB
const PING_MS = 25 * 1000;
const PHONES_PER_PC = 3;
const PHONE_MSGS_PER_SEC = 40;         // token bucket per phone
const TOKEN_RE = /^[A-Z2-7]{16}$/;     // base32, 80 bits
const PC_ID_RE = /^[A-Z2-7]{16,32}$/;

const PUBLIC_DIR = path.join(__dirname, 'public');
// Browser builds of TensorFlow.js, served from our own origin.
const VENDOR = {
  'tf-core.min.js': '@tensorflow/tfjs-core/dist/tf-core.min.js',
  'tf-converter.min.js': '@tensorflow/tfjs-converter/dist/tf-converter.min.js',
  'tf-backend-webgl.min.js': '@tensorflow/tfjs-backend-webgl/dist/tf-backend-webgl.min.js',
};
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
  '.bin': 'application/octet-stream',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
};

// ---------------------------------------------------------------- state

/** pcId -> { id, keyHash, ws, token, phones: Map<sid, phone>, seq } */
const pcs = new Map();
/** token -> pc entry */
const tokens = new Map();

const sha256 = (s) => crypto.createHash('sha256').update(s).digest('hex');
const shortId = (id) => id.slice(0, 4) + '…';

function send(ws, obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function parse(data, isBinary) {
  if (isBinary) return null;
  try {
    const m = JSON.parse(data.toString('utf8'));
    return m && typeof m === 'object' && !Array.isArray(m) ? m : null;
  } catch {
    return null;
  }
}

function dropPc(pc, code, reason) {
  if (pcs.get(pc.id) === pc) pcs.delete(pc.id);
  if (pc.token && tokens.get(pc.token) === pc) tokens.delete(pc.token);
  pc.token = null;
  for (const phone of pc.phones.values()) phone.ws.close(code, reason);
  pc.phones.clear();
}

// ---------------------------------------------------------------- PC side

function onPc(ws) {
  let pc = null;

  ws.on('message', (data, isBinary) => {
    const m = parse(data, isBinary);
    if (!m) return ws.close(1007, 'bad message');

    if (!pc) {
      // First message: who is this PC. The id is random and never shown;
      // the key keeps somebody else from taking the id over while the
      // real PC is online.
      if (m.t !== 'hello' || !PC_ID_RE.test(m.pc || '') ||
          typeof m.key !== 'string' || m.key.length < 16 || m.key.length > 128) {
        return ws.close(4400, 'hello expected');
      }
      const keyHash = sha256(m.key);
      const old = pcs.get(m.pc);
      if (old && old.keyHash !== keyHash) return ws.close(4403, 'pc id in use');
      if (old) {
        dropPc(old, 4410, 'pc reconnected');
        old.ws.close(4409, 'replaced');
      }
      pc = { id: m.pc, keyHash, ws, token: null, phones: new Map(), seq: 0 };
      pcs.set(pc.id, pc);
      send(ws, { t: 'ready', v: 1 });
      console.log(`pc ${shortId(pc.id)} online (${pcs.size} total)`);
      return;
    }

    switch (m.t) {
      case 'offer': {
        // A new pairing token replaces the previous one. Phones that are
        // already connected stay connected.
        if (!TOKEN_RE.test(m.token || '')) return;
        if (tokens.has(m.token) && tokens.get(m.token) !== pc) return;   // astronomically unlikely
        if (pc.token) tokens.delete(pc.token);
        pc.token = m.token;
        tokens.set(m.token, pc);
        return;
      }
      case 'to': {
        const phone = pc.phones.get(m.sid);
        if (phone && m.msg && typeof m.msg === 'object') send(phone.ws, m.msg);
        return;
      }
      case 'kick': {
        const phone = pc.phones.get(m.sid);
        if (phone) phone.ws.close(4000, String(m.reason || 'bye').slice(0, 60));
        return;
      }
      case 'ping':
        // The PC's WinHTTP socket can't see WebSocket pings; this lets it
        // notice a dead connection on its own.
        send(ws, { t: 'pong' });
        return;
      default:
        return;
    }
  });

  ws.on('close', () => {
    if (!pc) return;
    if (pcs.get(pc.id) === pc) {
      dropPc(pc, 4410, 'pc offline');
      console.log(`pc ${shortId(pc.id)} offline (${pcs.size} total)`);
    }
  });
}

// ---------------------------------------------------------------- phone side

function onPhone(ws, token) {
  const pc = TOKEN_RE.test(token) ? tokens.get(token) : null;
  if (!pc) return ws.close(4404, 'expired');

  // One phone per token: reloading the page replaces the old connection.
  for (const other of pc.phones.values()) {
    if (other.token === token) other.ws.close(4001, 'replaced');
  }
  if (pc.phones.size >= PHONES_PER_PC) return ws.close(4429, 'too many phones');

  const sid = 'p' + (++pc.seq);
  const phone = { ws, token, sid, budget: PHONE_MSGS_PER_SEC, refill: Date.now() };
  pc.phones.set(sid, phone);
  send(pc.ws, { t: 'join', sid });

  ws.on('message', (data, isBinary) => {
    const now = Date.now();
    phone.budget = Math.min(PHONE_MSGS_PER_SEC,
      phone.budget + ((now - phone.refill) / 1000) * PHONE_MSGS_PER_SEC);
    phone.refill = now;
    if (phone.budget < 1) return;          // drop floods silently
    phone.budget -= 1;

    const m = parse(data, isBinary);
    if (m) send(pc.ws, { t: 'from', sid, msg: m });
  });

  ws.on('close', () => {
    if (pc.phones.get(sid) === phone) {
      pc.phones.delete(sid);
      send(pc.ws, { t: 'leave', sid });
    }
  });
}

// ---------------------------------------------------------------- HTTP

const SECURITY_HEADERS = {
  'X-Content-Type-Options': 'nosniff',
  // The pairing token is in the page URL: never leak it in a Referer.
  'Referrer-Policy': 'no-referrer',
  'Permissions-Policy': 'camera=(self), microphone=()',
  'Content-Security-Policy':
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; " +
    "media-src 'self' blob:; connect-src 'self' wss: https:; frame-ancestors 'none'",
};

function sendFile(res, file, cacheControl) {
  fs.readFile(file, (err, body) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8', ...SECURITY_HEADERS });
      return res.end('not found');
    }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file)] || 'application/octet-stream',
      'Cache-Control': cacheControl,
      ...SECURITY_HEADERS,
    });
    res.end(body);
  });
}

const server = http.createServer((req, res) => {
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.writeHead(405, SECURITY_HEADERS);
    return res.end();
  }
  const url = new URL(req.url, 'http://x');
  const p = url.pathname;

  if (p === '/healthz') {
    res.writeHead(200, { 'Content-Type': 'text/plain', ...SECURITY_HEADERS });
    return res.end(`ok ${pcs.size}`);
  }
  // The QR code is upper case (smaller QR): /S/ and /s/ are the same page.
  if (/^\/s\/[A-Za-z2-7]{16}$/i.test(p) || p === '/demo') {
    return sendFile(res, path.join(PUBLIC_DIR, 'phone.html'), 'no-store');
  }
  if (p === '/' || p === '/index.html') {
    return sendFile(res, path.join(PUBLIC_DIR, 'index.html'), 'no-cache');
  }
  if (p.startsWith('/vendor/')) {
    const rel = VENDOR[p.slice('/vendor/'.length)];
    if (rel) return sendFile(res, path.join(__dirname, 'node_modules', rel), 'public, max-age=31536000, immutable');
  }
  if (p.startsWith('/static/')) {
    const file = path.normalize(path.join(PUBLIC_DIR, p.slice('/static/'.length)));
    if (file.startsWith(PUBLIC_DIR + path.sep)) return sendFile(res, file, 'no-cache');
  }
  res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8', ...SECURITY_HEADERS });
  res.end('not found');
});

const wss = new WebSocketServer({ noServer: true, maxPayload: MAX_MSG_BYTES });

server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url, 'http://x');
  let handler = null;
  if (url.pathname === '/ws/pc') handler = (ws) => onPc(ws);
  else if (url.pathname === '/ws/phone') {
    const token = (url.searchParams.get('token') || '').toUpperCase();
    handler = (ws) => onPhone(ws, token);
  }
  if (!handler) return socket.destroy();
  wss.handleUpgrade(req, socket, head, (ws) => {
    ws.isAlive = true;
    ws.on('pong', () => { ws.isAlive = true; });
    ws.on('error', () => {});
    handler(ws);
  });
});

// Drop connections that stopped answering pings (phone screen off, PC
// asleep, dead NAT mapping) so their tokens don't linger.
setInterval(() => {
  for (const ws of wss.clients) {
    if (!ws.isAlive) { ws.terminate(); continue; }
    ws.isAlive = false;
    ws.ping();
  }
}, PING_MS).unref();

for (const rel of Object.values(VENDOR)) {
  if (!fs.existsSync(path.join(__dirname, 'node_modules', rel))) {
    console.warn(`missing ${rel} - run npm install`);
  }
}

server.listen(PORT, () => console.log(`exercise-unlock relay listening on :${PORT}`));

module.exports = { server };   // for tests
