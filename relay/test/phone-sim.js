'use strict';
// A pretend phone, for trying the PC side without one: joins a relay the way
// the page does (/ws/phone?token=...) and streams synthetic poses (synth.js)
// in the page's format, ~12 a second.
//
//   node test/phone-sim.js <relay URL> <TOKEN> [squats|pushups|situps|stand]
//
// The service's QR code (counting): the exercise given, reps logged as the
// PC answers them. The calibration wizard's ("calib" mode): the pose follows
// its step - each photo gets its held pose, each check its exercise.
const WebSocket = require('ws');
const S = require('../public/synth.js');

const [base, token, what = 'squats'] = process.argv.slice(2);
if (!base || !token) {
  console.error('usage: node test/phone-sim.js <relay URL> <TOKEN> [squats|pushups|situps|stand]');
  process.exit(2);
}
const url = base.replace(/^http/i, 'ws').replace(/\/+$/, '') + '/ws/phone?token=' + encodeURIComponent(token);
const W = 1280, H = 720, FRAME_MS = 85;
const t0 = Date.now();

const squats = (t) => S.squatPose(S.cycleAngle(t, 2000, 175, 88));
const pushups = (t) => S.pushupPose(S.cycleAngle(t, 1800, 172, 95));
const situps = (t) => S.situpPose(S.cycleAngle(t, 2000, 135, 45));
const stand = () => S.squatPose(176);
const HELD = { 1: () => S.squatPose(176), 2: () => S.squatPose(92), 3: () => S.pushupPose(172),
               4: () => S.pushupPose(95), 5: () => S.situpPose(135), 6: () => S.situpPose(45) };
const CYCLES = { squat: squats, pushup: pushups, situp: situps };

let config = null, step = null, timer = null;

function poseAt(t) {
  if (config && config.mode === 'calib') {
    if (step && step.shot && HELD[step.shot]) return HELD[step.shot]();
    if (step && step.test && CYCLES[step.test]) return CYCLES[step.test](t);
    return stand();
  }
  return ({ squats, pushups, situps, stand })[what](t);
}

const ws = new WebSocket(url);
const send = (m) => ws.readyState === WebSocket.OPEN && ws.send(JSON.stringify(m));

ws.on('open', () => {
  console.log('connected');
  send({ t: 'hello', lang: 'en', ua: 'phone-sim' });
});
ws.on('message', (data) => {
  let m;
  try { m = JSON.parse(data); } catch { return; }
  switch (m.t) {
    case 'config':
      config = m;
      console.log(`config mode=${m.mode || 'count'} poses=${m.poses ? 1 : 0} space=${m.space || '-'}`);
      if (!m.poses) console.log('this PC does not count from poses');
      clearInterval(timer);
      timer = setInterval(() => {
        const t = Date.now() - t0;
        const k = [];
        for (const j of poseAt(t)) k.push(Math.round(j.x * 10000), Math.round(j.y * 10000), Math.round(j.c * 100));
        send({ t: 'pose', ts: t, w: W, h: H, k });
      }, FRAME_MS);
      break;
    case 'calib':
      step = m;
      console.log(`step: ${m.title} (photo ${m.shot || '-'}, check ${m.test || '-'}${m.busy ? ', busy' : ''})`);
      break;
    case 'result':
      console.log(`result ${m.ex} ${m.ok ? 'COUNTED' : 'not counted: ' + m.why}`);
      break;
    case 'status':
      console.log(`status ${JSON.stringify(m.counts)} earned ${m.earned}/${m.min}${m.active === false ? ' (inactive: ' + m.why + ')' : ''}`);
      break;
    case 'bye':
      console.log('bye: ' + m.reason);
      break;
    default:
      console.log('<- ' + JSON.stringify(m).slice(0, 120));
  }
});
ws.on('close', (code, reason) => {
  console.log(`closed ${code} ${reason}`);
  clearInterval(timer);
  process.exit(0);
});
ws.on('error', (err) => {
  console.error('error: ' + err.message);
  process.exit(1);
});
