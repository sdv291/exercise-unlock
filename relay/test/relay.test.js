'use strict';
// Relay integration test: a real server on a random port, fake PCs and
// phones as WebSocket clients.
process.env.PORT = '0';

const test = require('node:test');
const assert = require('node:assert');
const { WebSocket } = require('ws');
const { server } = require('../server.js');

let base = '';
test.before(() => new Promise((resolve) => {
  const ready = () => { base = `127.0.0.1:${server.address().port}`; resolve(); };
  if (server.listening) ready(); else server.once('listening', ready);
}));
test.after(() => {
  server.closeAllConnections();
  server.close();
  setTimeout(() => process.exit(0), 100).unref();   // the ping interval timer is unref'd anyway
});

// ---- helpers

const PC_ID = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.slice(0, 26);
const KEY = 'K'.repeat(32);
const TOKEN = 'QWERTYUIOPASDFGH';

// WebSocket with a message inbox and a close promise.
function client(path) {
  const ws = new WebSocket(`ws://${base}${path}`);
  ws.inbox = [];
  ws.waiters = [];
  ws.on('message', (data) => {
    const m = JSON.parse(data.toString());
    const i = ws.waiters.findIndex((w) => w.pred(m));
    if (i >= 0) ws.waiters.splice(i, 1)[0].resolve(m); else ws.inbox.push(m);
  });
  ws.closed = new Promise((resolve) => ws.on('close', (code, reason) => resolve({ code, reason: reason.toString() })));
  ws.opened = new Promise((resolve, reject) => { ws.on('open', resolve); ws.on('error', reject); });
  return ws;
}
function next(ws, pred = () => true, ms = 2000) {
  const i = ws.inbox.findIndex(pred);
  if (i >= 0) return Promise.resolve(ws.inbox.splice(i, 1)[0]);
  return new Promise((resolve, reject) => {
    const w = { pred, resolve };
    ws.waiters.push(w);
    setTimeout(() => { ws.waiters = ws.waiters.filter((x) => x !== w); reject(new Error('timeout waiting for message')); }, ms);
  });
}
const is = (t) => (m) => m.t === t;
const send = (ws, obj) => ws.send(JSON.stringify(obj));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function pc(id = PC_ID, key = KEY, token = TOKEN) {
  const ws = client('/ws/pc');
  await ws.opened;
  send(ws, { t: 'hello', pc: id, key, v: 1 });
  await next(ws, is('ready'));
  if (token) send(ws, { t: 'offer', token });
  await sleep(50);
  return ws;
}
async function phone(token = TOKEN) {
  const ws = client(`/ws/phone?token=${token}`);
  await ws.opened;
  return ws;
}
async function get(path, method = 'GET') {
  const res = await fetch(`http://${base}${path}`, { method });
  return { status: res.status, headers: res.headers, body: await res.text() };
}

// ---- HTTP

test('http: health, pages, static files, vendor bundles', async () => {
  assert.match((await get('/healthz')).body, /^ok \d+$/);
  for (const p of ['/s/' + TOKEN, '/s/' + TOKEN.toLowerCase(), '/S/' + TOKEN, '/demo']) {
    const r = await get(p);
    assert.strictEqual(r.status, 200, p);
    assert.match(r.body, /\/static\/phone\.js/, p);
    assert.strictEqual(r.headers.get('referrer-policy'), 'no-referrer');
    assert.match(r.headers.get('content-security-policy'), /default-src 'self'/);
    assert.strictEqual(r.headers.get('cache-control'), 'no-store');
  }
  assert.strictEqual((await get('/')).status, 200);
  const css = await get('/static/style.css');
  assert.strictEqual(css.status, 200);
  assert.match(css.headers.get('content-type'), /text\/css/);
  for (const v of ['tf-core.min.js', 'tf-converter.min.js', 'tf-backend-webgl.min.js']) {
    const r = await get('/vendor/' + v);
    assert.strictEqual(r.status, 200, v);
    assert.ok(r.body.length > 10000, v + ' has content');
  }
});

test('http: no way out of public/, wrong methods refused', async () => {
  for (const p of ['/static/../server.js', '/static/%2e%2e/server.js', '/static/..%2fserver.js',
                   '/static/..\\server.js', '/vendor/../package.json', '/s/TOOSHORT', '/s/' + TOKEN + 'X',
                   '/node_modules/ws/package.json', '/server.js']) {
    assert.strictEqual((await get(p)).status, 404, p);
  }
  assert.strictEqual((await get('/healthz', 'POST')).status, 405);
});

// ---- WebSocket protocol

test('pc must say hello first', async () => {
  const ws = client('/ws/pc');
  await ws.opened;
  send(ws, { t: 'offer', token: TOKEN });
  assert.strictEqual((await ws.closed).code, 4400);
});

test('unknown token is refused', async () => {
  const ws = client('/ws/phone?token=AAAAAAAAAAAAAAAA');
  assert.strictEqual((await ws.closed).code, 4404);
});

test('phone and pc talk through the relay', async () => {
  const p = await pc();
  const ph = await phone(TOKEN.toLowerCase());          // case-insensitive token
  const join = await next(p, is('join'));
  assert.ok(join.sid);

  send(ph, { t: 'rep', ex: 'squat', n: 1 });
  const from = await next(p, is('from'));
  assert.deepStrictEqual(from, { t: 'from', sid: join.sid, msg: { t: 'rep', ex: 'squat', n: 1 } });

  send(p, { t: 'to', sid: join.sid, msg: { t: 'status', counts: { squat: 1 } } });
  assert.deepStrictEqual(await next(ph), { t: 'status', counts: { squat: 1 } });

  send(p, { t: 'ping' });
  await next(p, is('pong'));

  ph.close();
  assert.strictEqual((await next(p, is('leave'))).sid, join.sid);
  p.close();
});

test('reloading the page replaces the old phone connection', async () => {
  const p = await pc();
  const a = await phone();
  const ja = await next(p, is('join'));
  const b = await phone();
  assert.strictEqual((await a.closed).code, 4001);
  const jb = await next(p, is('join'));
  assert.notStrictEqual(ja.sid, jb.sid);
  assert.strictEqual((await next(p, is('leave'))).sid, ja.sid);
  send(b, { t: 'hello' });
  assert.strictEqual((await next(p, is('from'))).sid, jb.sid);
  p.close();
});

test('a new token: connected phone stays, old QR stops working', async () => {
  const p = await pc();
  const a = await phone();
  await next(p, is('join'));
  send(p, { t: 'offer', token: 'ZXCVBNMLKJHGFDSA' });
  await sleep(50);
  const stale = client(`/ws/phone?token=${TOKEN}`);
  assert.strictEqual((await stale.closed).code, 4404);
  send(a, { t: 'hello' });
  await next(p, is('from'));                            // still connected
  const fresh = await phone('ZXCVBNMLKJHGFDSA');
  await next(p, is('join'));
  fresh.close();
  p.close();
  assert.strictEqual((await a.closed).code, 4410);     // pc gone -> phone told
});

test('pc kick closes the phone', async () => {
  const p = await pc();
  const ph = await phone();
  const { sid } = await next(p, is('join'));
  send(p, { t: 'kick', sid, reason: 'unlocked' });
  const c = await ph.closed;
  assert.strictEqual(c.code, 4000);
  assert.strictEqual(c.reason, 'unlocked');
  p.close();
});

test('pc id: same key replaces, other key is refused', async () => {
  const p1 = await pc();
  const ph = await phone();
  await next(p1, is('join'));
  const intruder = client('/ws/pc');
  await intruder.opened;
  send(intruder, { t: 'hello', pc: PC_ID, key: 'X'.repeat(32), v: 1 });
  assert.strictEqual((await intruder.closed).code, 4403);

  const p2 = await pc(PC_ID, KEY, 'MNBVCXZLKJHGFDSA');  // same PC reconnecting
  assert.strictEqual((await p1.closed).code, 4409);
  assert.strictEqual((await ph.closed).code, 4410);
  p2.close();
});

test('phone flood is rate limited', async () => {
  const p = await pc();
  const ph = await phone();
  await next(p, is('join'));
  for (let i = 0; i < 300; i++) send(ph, { t: 'rep', n: i });
  await sleep(500);
  const got = p.inbox.filter(is('from')).length;
  assert.ok(got >= 30 && got <= 70, `forwarded ${got} of 300`);
  p.close();
});

test('oversized and binary messages', async () => {
  const p = await pc();
  const ph = await phone();
  await next(p, is('join'));
  ph.send(Buffer.from([1, 2, 3]));                      // binary: ignored
  send(ph, { t: 'ok' });
  assert.deepStrictEqual((await next(p, is('from'))).msg, { t: 'ok' });
  ph.send('x'.repeat(20000));                           // > 8 KB: connection dropped
  assert.strictEqual((await ph.closed).code, 1009);
  p.close();
});

test('max three phones per pc', async () => {
  const p = await pc(PC_ID, KEY, null);
  const phones = [];
  for (const t of ['AAAAAAAAAAAAAAA2', 'AAAAAAAAAAAAAAA3', 'AAAAAAAAAAAAAAA4']) {
    send(p, { t: 'offer', token: t });
    await sleep(30);
    phones.push(await phone(t));
    await next(p, is('join'));
  }
  send(p, { t: 'offer', token: 'AAAAAAAAAAAAAAA5' });
  await sleep(30);
  const fourth = client('/ws/phone?token=AAAAAAAAAAAAAAA5');
  assert.strictEqual((await fourth.closed).code, 4429);
  p.close();
});
