// Exercise Unlock - phone camera page.
//
// Opened from the QR code on the PC's lock screen (/s/<TOKEN>). Talks to
// the PC through the relay (/ws/phone): gets the exercises and calibrated
// thresholds, runs MoveNet on the camera, counts reps with the same
// detectors as the PC (exercises.js) and reports each rep. The PC credits
// it and answers with the running totals.
//
// /demo?demo=squat, =pushup or =situp feeds synthetic poses instead of
// the camera - only without a PC, for trying the page out.

(function () {
  'use strict';

  const EU = window.EU;
  const FRAME_MS = 85;               // ~12 poses/s, the rate the PC detectors are tuned for
  const MODEL_URL = 'https://tfhub.dev/google/tfjs-model/movenet/singlepose/lightning/4';
  const MODEL_CACHE = 'indexeddb://exercise-unlock-movenet-lightning-4';
  const INPUT = 192;

  // ------------------------------------------------------------ text

  const TEXT = {
    uk: {
      connecting: 'Підключення до комп\'ютера…',
      start: 'Почати',
      placeHint: 'Постав телефон на підлогу або стілець за 2–3 м так, щоб тебе було видно збоку повністю — від голови до ніг.',
      loadingModel: 'Завантаження розпізнавання рухів…',
      cameraDenied: 'Немає доступу до камери. Дозволь камеру для цього сайту в налаштуваннях браузера й онови сторінку.',
      noCamera: 'Камеру не знайдено.',
      modelFailed: 'Не вдалося завантажити розпізнавання рухів. Перевір інтернет і онови сторінку.',
      expired: 'QR-код застарів. Відскануй новий на екрані блокування комп\'ютера.',
      pcOffline: 'Комп\'ютер не на екрані блокування. Заблокуй його (Win+L) і відскануй QR-код ще раз.',
      replaced: 'Сторінку відкрито на іншому телефоні або вкладці.',
      connLost: 'Зв\'язок втрачено, підключаюся знову…',
      seeNone: 'Стань так, щоб тебе було видно повністю',
      seeVertical: 'Бачу тебе — присідай!',
      seeHorizontal: 'Упор лежачи — віджимайся!',
      seeSitUp: 'Лежиш на спині — качай прес!',
      inactiveHours: 'Зараз поза дозволеним часом — вправи не зараховуються',
      inactiveSession: 'Комп\'ютер уже розблоковано',
      inactive: 'Вправи зараз не зараховуються',
      earned: 'Зароблено {0} з {1}',
      enough: 'Досить! Можна розблокувати комп\'ютер',
      unlockedTitle: 'Готово!',
      unlocked: 'Комп\'ютер розблоковано. Молодець!',
      endedTitle: 'Сеанс завершено',
      ended: 'Щоб продовжити, відскануй QR-код на екрані блокування ще раз.',
      flip: 'Камера',
      drop_timing: 'Не зараховано: занадто швидко або повільно',
      drop_depth: 'Не зараховано: глибше!',
      drop_drift: 'Не зараховано: стій на місці',
      drop_hip: 'Не зараховано: присідай глибше',
      drop_shaky: 'Не зараховано: рівніше',
      rejected: 'Комп\'ютер не зарахував: {0}',
      ex_squat: 'Присідання',
      ex_pushup: 'Віджимання',
      ex_situp: 'Прес',
      demo: 'Демо без камери',
    },
    en: {
      connecting: 'Connecting to the computer…',
      start: 'Start',
      placeHint: 'Put the phone on the floor or a chair 2–3 m away so it sees you from the side, head to feet.',
      loadingModel: 'Loading motion recognition…',
      cameraDenied: 'No camera access. Allow the camera for this site in the browser settings and reload.',
      noCamera: 'No camera found.',
      modelFailed: 'Could not load motion recognition. Check the internet connection and reload.',
      expired: 'This QR code has expired. Scan the new one on the computer\'s lock screen.',
      pcOffline: 'The computer is not on its lock screen. Lock it (Win+L) and scan the QR code again.',
      replaced: 'This page was opened on another phone or tab.',
      connLost: 'Connection lost, reconnecting…',
      seeNone: 'Stand where the camera sees all of you',
      seeVertical: 'I can see you - squat!',
      seeHorizontal: 'Plank position - push up!',
      seeSitUp: 'On your back - sit up!',
      inactiveHours: 'Outside the allowed hours - exercises don\'t count now',
      inactiveSession: 'The computer is already unlocked',
      inactive: 'Exercises don\'t count right now',
      earned: 'Earned {0} of {1}',
      enough: 'Enough! You can unlock the computer',
      unlockedTitle: 'Done!',
      unlocked: 'The computer is unlocked. Well done!',
      endedTitle: 'Session ended',
      ended: 'To continue, scan the QR code on the lock screen again.',
      flip: 'Camera',
      drop_timing: 'Not counted: too fast or too slow',
      drop_depth: 'Not counted: go deeper!',
      drop_drift: 'Not counted: stay in place',
      drop_hip: 'Not counted: squat deeper',
      drop_shaky: 'Not counted: smoother',
      rejected: 'The computer didn\'t count it: {0}',
      ex_squat: 'Squats',
      ex_pushup: 'Push-ups',
      ex_situp: 'Sit-ups',
      demo: 'Demo without camera',
    },
  };
  const lang = /^(uk|ru|be)/i.test(navigator.language || '') ? 'uk' : 'en';
  function T(key, ...args) {
    const s = (TEXT[lang][key] !== undefined ? TEXT[lang][key] : TEXT.en[key]) || key;
    return s.replace(/\{(\d)\}/g, (_, i) => String(args[Number(i)]));
  }
  document.documentElement.lang = lang;

  // ------------------------------------------------------------ DOM

  const $ = (id) => document.getElementById(id);
  const ui = {
    start: $('screen-start'), live: $('screen-live'), end: $('screen-end'),
    startMsg: $('start-msg'), startBtn: $('btn-start'), hint: $('place-hint'),
    video: $('video'), canvas: $('overlay'), banner: $('banner'), toast: $('toast'), debug: $('debug'),
    counters: $('counters'), barFill: $('bar-fill'), earned: $('earned-text'),
    flip: $('btn-flip'), endTitle: $('end-title'), endMsg: $('end-msg'),
  };
  ui.hint.textContent = T('placeHint');
  ui.startBtn.textContent = T('start');
  ui.flip.textContent = T('flip');
  ui.startMsg.textContent = T('connecting');

  function show(screen) {
    for (const s of [ui.start, ui.live, ui.end]) s.hidden = s !== screen;
  }
  let toastTimer = 0;
  function toast(text) {
    ui.toast.textContent = text;
    ui.toast.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { ui.toast.hidden = true; }, 2200);
  }
  function banner(text, kind) {
    ui.banner.textContent = text;
    ui.banner.className = 'banner ' + (kind || '');
  }
  function finish(title, msg) {
    ended = true;
    stopCamera();
    ui.endTitle.textContent = title;
    ui.endMsg.textContent = msg;
    show(ui.end);
    if (ws) ws.close();
  }

  // ------------------------------------------------------------ sound

  let audio = null;
  function beep(freq, ms, delay = 0) {
    if (!audio) return;
    const t = audio.currentTime + delay / 1000;
    const osc = audio.createOscillator(), gain = audio.createGain();
    osc.frequency.value = freq;
    gain.gain.setValueAtTime(0.0001, t);
    gain.gain.exponentialRampToValueAtTime(0.4, t + 0.01);
    gain.gain.exponentialRampToValueAtTime(0.0001, t + ms / 1000);
    osc.connect(gain).connect(audio.destination);
    osc.start(t);
    osc.stop(t + ms / 1000 + 0.02);
  }
  const sounds = {
    rep() { beep(880, 140); beep(1320, 180, 120); if (navigator.vibrate) navigator.vibrate(60); },
    ready() { beep(660, 120); beep(990, 160, 140); },
    drop() { beep(220, 220); },
    enough() { beep(784, 150); beep(988, 150, 160); beep(1319, 300, 320); },
  };

  // ------------------------------------------------------------ connection

  const token = (location.pathname.split('/')[2] || '').toUpperCase();
  // Demo poses only without a PC: on a real pairing they'd be free reps.
  const demo = /^[A-Z2-7]{16}$/.test(token) ? null
             : new URLSearchParams(location.search).get('demo');   // 'squat' | 'pushup' | null
  const wsUrl = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host +
                '/ws/phone?token=' + encodeURIComponent(token);

  let ws = null, retry = 0, ended = false;
  let config = null;          // {exercises:[{id, params}], aspect, ...} from the PC
  let status = null;          // last {t:'status'} from the PC
  let seq = 0;

  function sendMsg(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
  }

  function connect() {
    ws = new WebSocket(wsUrl);
    ws.onopen = () => {
      retry = 0;
      sendMsg({ t: 'hello', lang, ua: navigator.userAgent.slice(0, 120) });
      if (running) banner('', '');
    };
    ws.onmessage = (e) => {
      let m;
      try { m = JSON.parse(e.data); } catch { return; }
      onMessage(m);
    };
    ws.onclose = (e) => {
      if (ended) return;
      if (e.code === 4404) return finish(T('endedTitle'), T('expired'));
      if (e.code === 4410) return finish(T('endedTitle'), T('pcOffline'));
      if (e.code === 4001) return finish(T('endedTitle'), T('replaced'));
      if (e.code === 4000) return finish(T('endedTitle'), T('ended'));
      if (running) banner(T('connLost'), 'warn');
      else ui.startMsg.textContent = T('connLost');
      setTimeout(connect, Math.min(10000, 1000 * Math.pow(2, retry++)));
    };
  }

  function onMessage(m) {
    switch (m.t) {
      case 'config':
        config = m;
        buildCounters();
        coach = new EU.Coach(config.exercises || []);
        if (!running) {
          ui.startMsg.textContent = '';
          ui.startBtn.hidden = false;
        }
        break;
      case 'status': {
        const wasEnough = status && status.enough;
        status = m;
        renderStatus();
        if (m.enough && !wasEnough && running) sounds.enough();
        break;
      }
      case 'reject':
        toast(T('rejected', m.why || '?'));
        break;
      case 'bye':
        finish(m.reason === 'unlocked' ? T('unlockedTitle') : T('endedTitle'),
               m.reason === 'unlocked' ? T('unlocked') : T('ended'));
        break;
      default:
        break;
    }
  }

  // ------------------------------------------------------------ counters UI

  function buildCounters() {
    ui.counters.textContent = '';
    for (const e of (config && config.exercises) || []) {
      if (!EU.EXERCISES[e.id]) continue;
      const box = document.createElement('div');
      box.className = 'counter';
      box.id = 'counter-' + e.id;
      const n = document.createElement('b');
      n.textContent = '0';
      const label = document.createElement('span');
      label.textContent = T('ex_' + e.id);
      box.append(n, label);
      ui.counters.append(box);
    }
  }

  const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

  function renderStatus() {
    if (!status) return;
    for (const [id, n] of Object.entries(status.counts || {})) {
      const box = $('counter-' + id);
      if (box) box.firstChild.textContent = String(n);
    }
    const earned = status.earned || 0, min = status.min || 0;
    ui.barFill.style.width = (min > 0 ? Math.min(100, (earned / min) * 100) : 100) + '%';
    ui.earned.textContent = status.enough ? T('enough') : T('earned', mmss(earned), mmss(min));
    ui.earned.className = status.enough ? 'ok' : '';
  }

  // Count up right away; the PC's next status has the authoritative total.
  function bump(id) {
    const box = $('counter-' + id);
    if (!box) return;
    box.firstChild.textContent = String((parseInt(box.firstChild.textContent, 10) || 0) + 1);
    box.classList.remove('bump');
    void box.offsetWidth;            // restart the animation
    box.classList.add('bump');
  }

  // ------------------------------------------------------------ camera + model

  let stream = null, model = null, running = false, facing = 'user', coach = null;
  let lastPosture = EU.Posture.None, lastDropToast = 0;

  async function startCamera() {
    stopCamera();
    stream = await navigator.mediaDevices.getUserMedia({
      audio: false,
      video: { facingMode: facing, width: { ideal: 1280 }, height: { ideal: 720 } },
    });
    ui.video.srcObject = stream;
    ui.video.classList.toggle('mirror', facing === 'user');
    await ui.video.play();
  }
  function stopCamera() {
    running = false;
    if (stream) for (const tr of stream.getTracks()) tr.stop();
    stream = null;
  }

  async function loadModel() {
    await tf.setBackend('webgl').catch(() => tf.setBackend('cpu'));
    await tf.ready();
    try {
      model = await tf.loadGraphModel(MODEL_CACHE);
    } catch {
      model = await tf.loadGraphModel(MODEL_URL, { fromTFHub: true });
      model.save(MODEL_CACHE).catch(() => {});      // next time: no download
    }
    // Warm-up so the first real frame isn't a 1-second stall.
    tf.tidy(() => model.execute(tf.zeros([1, INPUT, INPUT, 3], 'int32')));
  }

  // MoveNet on the current video frame. The frame is letterboxed to a
  // square (better accuracy than the PC's stretch); the result is mapped
  // back to frame-normalized coordinates.
  async function infer() {
    const W = ui.video.videoWidth, H = ui.video.videoHeight;
    if (!W || !H) return null;
    const S = Math.max(W, H);
    const top = Math.floor((S - H) / 2), left = Math.floor((S - W) / 2);
    const out = tf.tidy(() => {
      const img = tf.browser.fromPixels(ui.video);
      const sq = tf.pad(img, [[top, S - H - top], [left, S - W - left], [0, 0]]);
      // Function form only: the tfjs-core bundle has no chained tensor
      // methods (x.expandDims() is undefined there).
      const input = tf.expandDims(tf.cast(tf.image.resizeBilinear(sq, [INPUT, INPUT]), 'int32'), 0);
      return model.execute(input);
    });
    const d = await out.data();
    out.dispose();
    const pose = [];
    for (let i = 0; i < 17; i++) {
      pose.push({ x: (d[i * 3 + 1] * S - left) / W, y: (d[i * 3] * S - top) / H, c: d[i * 3 + 2] });
    }
    return { pose, W, H };
  }

  // Frame-normalized -> the PC camera's convention. The PC measures angles
  // on x / frameWidth, y / frameHeight of its own camera, so its calibrated
  // thresholds include that camera's aspect ratio. Rescale x so that a
  // pixel distance counts the same here as it would there.
  function toPcSpace(pose, W, H) {
    const aspect = (config && config.aspect) || 16 / 9;
    const k = W / (H * aspect);
    return pose.map((j) => ({ x: j.x * k, y: j.y, c: j.c }));
  }

  // ------------------------------------------------------------ failure reports

  // The phone has no log of its own: problems go to the PC's service.log
  // (as {t:'diag'}) and onto the screen.
  const failures = { count: 0, sentAt: 0, noVideo: 0, firstFrame: false, startedAt: 0 };
  function report(kind, fields) {
    sendMsg(Object.assign({ t: 'diag', kind }, fields));
  }
  function reportError(where, err) {
    failures.count++;
    const msg = String((err && (err.message || err.name)) || err || '?').slice(0, 120);
    const now = performance.now();
    if (failures.count === 1 || now - failures.sentAt > 10000) {
      failures.sentAt = now;
      report('error', { where, msg, n: failures.count });
    }
    if (running) banner('⚠ ' + msg, 'warn');
    console.error(where, err);
  }
  window.addEventListener('error', (e) => reportError('page', e.error || e.message));
  window.addEventListener('unhandledrejection', (e) => reportError('promise', e.reason));

  // A timer, not requestAnimationFrame: ~12 poses/s is all the detectors
  // want, and the phone needn't wake up 60-120 times a second for it.
  let loopTimer = 0;
  function startLoop() {
    clearTimeout(loopTimer);
    running = true;
    tick();
  }
  async function tick() {
    if (!running) return;
    const now = performance.now();
    try {
      let frame;
      if (demo) {
        const pose = demo === 'pushup' ? EUSynth.pushupPose(EUSynth.cycleAngle(now, 1800, 172, 95))
                   : demo === 'situp' ? EUSynth.situpPose(EUSynth.cycleAngle(now, 2000, 135, 45))
                   : EUSynth.squatPose(EUSynth.cycleAngle(now, 2000, 175, 88));
        frame = { pose, pcPose: pose, W: 1, H: 1 };
      } else {
        const r = await infer();
        if (!r) {
          // Camera on but no picture size yet (or ever).
          if (++failures.noVideo === 1 || failures.noVideo % 120 === 0) {
            report('novideo', { n: failures.noVideo, w: ui.video.videoWidth, h: ui.video.videoHeight,
                                ready: ui.video.readyState, paused: ui.video.paused });
          }
          return;
        }
        frame = { pose: r.pose, pcPose: toPcSpace(r.pose, r.W, r.H), W: r.W, H: r.H };
        if (!failures.firstFrame) {
          failures.firstFrame = true;
          report('firstframe', { ms: Math.round(now - failures.startedAt), w: r.W, h: r.H });
        }
      }
      process(frame, now);
      draw(frame);
    } catch (err) {
      reportError('frame', err);
    } finally {
      if (running) loopTimer = setTimeout(tick, Math.max(0, FRAME_MS - (performance.now() - now)));
    }
  }

  function countingActive() {
    return !status || status.active !== false;
  }

  function process(frame, now) {
    if (!coach) return;
    if (!countingActive()) {
      // Same as the service: detectors are frozen, nothing accumulates.
      banner(status.why === 'hours' ? T('inactiveHours')
           : status.why === 'session' ? T('inactiveSession') : T('inactive'), 'warn');
      diagnose(frame, now, null);
      return;
    }
    const r = coach.update(frame.pcPose, now);
    diagnose(frame, now, r);
    if (r.posture !== lastPosture) {
      lastPosture = r.posture;
      if (r.posture !== EU.Posture.None) sounds.ready();
    }
    // While the connection is down keep its warning up.
    if (!ws || ws.readyState === WebSocket.OPEN) {
      banner(r.posture === EU.Posture.Vertical ? T('seeVertical')
           : r.posture === EU.Posture.Horizontal ? T('seeHorizontal')
           : r.posture === EU.Posture.SitUp ? T('seeSitUp') : T('seeNone'),
             r.posture === EU.Posture.None ? '' : 'go');
    }
    for (const rep of r.reps) {
      seq++;
      sendMsg({ t: 'rep', ex: rep.id, n: seq, q: Math.round(rep.quality * 100) / 100, dur: Math.round(rep.durMs) });
      sounds.rep();
      bump(rep.id);
    }
    if (r.drops.length && now - lastDropToast > 2500) {
      lastDropToast = now;
      toast(T('drop_' + r.drops[0].why));
      sounds.drop();
    }
    frame.angle = r.angle;
  }

  // ------------------------------------------------------------ diagnostics

  // Tap the video: everything that decides "counted or not", so a parent
  // can see why a rep didn't count (and send a screenshot).
  const PHASES = ['unknown', 'top', 'going down', 'down', 'going up', 'not in position'];
  const diag = { fps: 0, lastT: 0, angles: [], last: '', lastAt: 0, shownAt: 0,
                 posture: '', sentAt: 0, frames: 0, twoLegs: 0 };
  document.querySelector('.stage').addEventListener('click', () => {
    ui.debug.hidden = !ui.debug.hidden;
  });

  function diagnose(frame, now, r) {
    if (diag.lastT) {
      const fps = 1000 / Math.max(1, now - diag.lastT);
      diag.fps = diag.fps ? diag.fps * 0.8 + fps * 0.2 : fps;
    }
    diag.lastT = now;
    diag.angles = diag.angles.filter(([t]) => now - t < 4000);
    const range = () => {
      const a = diag.angles.map(([, v]) => v);
      return a.length ? [Math.round(Math.min(...a)), Math.round(Math.max(...a))] : [0, 0];
    };
    if (r) {
      if (r.active) diag.angles.push([now, r.angle]);
      for (const rep of r.reps) { diag.last = `${rep.id} counted (q ${rep.quality.toFixed(2)}, ${Math.round(rep.durMs)} ms)`; diag.lastAt = now; }
      for (const d of r.drops) {
        diag.last = `${d.id} NOT counted: ${d.why} (${Math.round(d.durMs)} ms)`;
        diag.lastAt = now;
        const [lo, hi] = range();
        sendMsg({ t: 'diag', kind: 'drop', ex: d.id, why: d.why, ms: Math.round(d.durMs), lo, hi });
      }
    }

    // The same facts, briefly, into the PC's service.log: posture changes,
    // dropped reps (above) and a summary every 10 s.
    const sq = coach.items.find((it) => it.id === 'squat');
    diag.frames++;
    if (sq && sq.detector.legs === 2) diag.twoLegs++;
    const posture = coach.posture.latched;
    if (posture !== diag.posture) {
      diag.posture = posture;
      sendMsg({ t: 'diag', kind: 'posture', posture, fps: Math.round(diag.fps) });
    }
    if (now - diag.sentAt >= 10000) {
      diag.sentAt = now;
      const J = EU.J, c = (i) => Math.round(frame.pose[i].c * 100) / 100;
      const active = coach.items.find((it) => it.id === (r && r.active));
      const [lo, hi] = range();
      sendMsg({
        t: 'diag', kind: 'tick', fps: Math.round(diag.fps * 10) / 10,
        backend: typeof tf !== 'undefined' ? tf.getBackend() : 'demo', w: frame.W, h: frame.H,
        posture, raw: coach.posture.raw || '', counting: countingActive(),
        ex: active ? active.id : '', phase: active ? PHASES[active.detector.phase] : '',
        angle: active ? Math.round(active.detector.angle) : 0, lo, hi,
        twoLegs: diag.frames ? Math.round((diag.twoLegs / diag.frames) * 100) / 100 : 0,
        conf: { lhip: c(J.lHip), lknee: c(J.lKnee), lankle: c(J.lAnkle), rhip: c(J.rHip), rknee: c(J.rKnee), rankle: c(J.rAnkle), nose: c(J.nose) },
      });
      diag.frames = 0;
      diag.twoLegs = 0;
    }
    if (ui.debug.hidden || now - diag.shownAt < 250) return;
    diag.shownAt = now;

    const f2 = (v) => v.toFixed(2).replace(/^0/, '');
    const conf = (i) => f2(frame.pose[i].c);
    const J = EU.J;
    const aspect = (config && config.aspect) || 16 / 9;
    const lines = [];
    lines.push(`${diag.fps.toFixed(1)} fps · ${typeof tf !== 'undefined' ? tf.getBackend() : 'demo'} · ` +
               `${frame.W}x${frame.H} · aspect ${aspect.toFixed(2)} · k ${(frame.W / (frame.H * aspect)).toFixed(2)}`);
    lines.push(`pose: ${coach.posture.latched} (raw ${coach.posture.raw || '-'}) · counting: ${countingActive() ? 'yes' : 'NO (' + (status && status.why) + ')'} · ` +
               `link: ${ws && ws.readyState === WebSocket.OPEN ? 'ok' : 'down'}`);
    const [lo, hi] = range();
    const span = diag.angles.length ? `4s ${lo}..${hi}°` : '';
    for (const it of coach.items) {
      const d = it.detector, p = d.p;
      if (it.id === 'squat') {
        lines.push(`squat: ${PHASES[d.phase] || d.phase} · knee ${Math.round(d.angle)}° ${it.id === (r && r.active) ? span : ''} · legs ${d.legs ?? 0}/2`);
        lines.push(`  top >=${p.standing} · start <${p.goingDown} · bottom <=${p.down} (2 legs) · up >${p.goingUp}`);
      } else if (it.id === 'pushup') {
        const g = d.gate || {};
        lines.push(`pushup: ${PHASES[d.phase] || d.phase} · elbow ${Math.round(d.angle)}° ${it.id === (r && r.active) ? span : ''} · plank ${Math.round(d.plank)}° (>=${p.plankMin})`);
        lines.push(`  arms ${g.arms ?? 0} · body ${g.plank ? 'ok' : 'no'} · hands down ${g.handsDown ? 'yes' : 'no'} · up >=${p.up} · bottom <=${p.down}`);
      } else if (it.id === 'situp') {
        lines.push(`situp: ${PHASES[d.phase] || d.phase} · hip ${Math.round(d.angle)}° ${it.id === (r && r.active) ? span : ''} · sides ${d.sides ?? 0}/2`);
        lines.push(`  lying >=${p.lying} · start <${p.rising} · top <=${p.sitting} · down >${p.lowering}`);
      }
    }
    lines.push(`L hip ${conf(J.lHip)} knee ${conf(J.lKnee)} ankle ${conf(J.lAnkle)} · ` +
               `R hip ${conf(J.rHip)} knee ${conf(J.rKnee)} ankle ${conf(J.rAnkle)} · nose ${conf(J.nose)}`);
    if (diag.last) lines.push(`last: ${diag.last}, ${Math.round((now - diag.lastAt) / 1000)} s ago`);
    ui.debug.textContent = lines.join('\n');
  }

  // ------------------------------------------------------------ drawing

  const BONES = [[5, 7], [7, 9], [6, 8], [8, 10], [5, 6], [5, 11], [6, 12], [11, 12],
                 [11, 13], [13, 15], [12, 14], [14, 16], [0, 5], [0, 6]];

  function draw(frame) {
    const cv = ui.canvas, rect = cv.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    if (cv.width !== Math.round(rect.width * dpr)) cv.width = Math.round(rect.width * dpr);
    if (cv.height !== Math.round(rect.height * dpr)) cv.height = Math.round(rect.height * dpr);
    const g = cv.getContext('2d');
    g.clearRect(0, 0, cv.width, cv.height);

    // Where the video sits inside the canvas (object-fit: contain).
    const vw = demo ? 16 : frame.W, vh = demo ? 9 : frame.H;
    const scale = Math.min(cv.width / vw, cv.height / vh);
    const ox = (cv.width - vw * scale) / 2, oy = (cv.height - vh * scale) / 2;
    const mirror = !demo && facing === 'user';
    // Camera poses are frame-normalized; demo poses are drawn over a 16:9 box.
    const P = (j) => [ox + (mirror ? 1 - j.x : j.x) * vw * scale, oy + j.y * vh * scale];

    g.lineWidth = 4 * dpr;
    g.lineCap = 'round';
    g.strokeStyle = '#2bff88';
    for (const [a, b] of BONES) {
      const ja = frame.pose[a], jb = frame.pose[b];
      if (ja.c < 0.2 || jb.c < 0.2) continue;
      const [x1, y1] = P(ja), [x2, y2] = P(jb);
      g.beginPath(); g.moveTo(x1, y1); g.lineTo(x2, y2); g.stroke();
    }
    g.fillStyle = '#fff';
    for (const j of frame.pose) {
      if (j.c < 0.2) continue;
      const [x, y] = P(j);
      g.beginPath(); g.arc(x, y, 4 * dpr, 0, Math.PI * 2); g.fill();
    }
    if (frame.angle) {
      g.font = `${18 * dpr}px system-ui, sans-serif`;
      g.fillStyle = '#ffe65a';
      g.fillText(Math.round(frame.angle) + '°', 12 * dpr, 28 * dpr);
    }
  }

  // ------------------------------------------------------------ start

  async function start() {
    ui.startBtn.disabled = true;
    try {
      audio = new (window.AudioContext || window.webkitAudioContext)();
    } catch { audio = null; }
    failures.startedAt = performance.now();
    try {
      if (!demo) {
        ui.startMsg.textContent = T('loadingModel');
        await startCamera();
        await loadModel();
        report('start', {
          backend: tf.getBackend(), webgl: tf.env().getNumber('WEBGL_VERSION'),
          w: ui.video.videoWidth, h: ui.video.videoHeight, facing,
          ms: Math.round(performance.now() - failures.startedAt),
        });
      }
    } catch (err) {
      console.error(err);
      report('startfail', { name: String(err && err.name), msg: String((err && err.message) || err).slice(0, 120) });
      ui.startBtn.disabled = false;
      const name = err && err.name;
      ui.startMsg.textContent = name === 'NotAllowedError' || name === 'SecurityError' ? T('cameraDenied')
        : name === 'NotFoundError' || name === 'OverconstrainedError' ? T('noCamera') : T('modelFailed');
      return;
    }
    if ('wakeLock' in navigator) navigator.wakeLock.request('screen').catch(() => {});
    show(ui.live);
    renderStatus();
    startLoop();
  }

  ui.startBtn.addEventListener('click', start);
  ui.flip.addEventListener('click', async () => {
    if (demo) return;
    facing = facing === 'user' ? 'environment' : 'user';
    try {
      await startCamera();
      startLoop();
    } catch (err) {
      console.error(err);
    }
  });
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && running && 'wakeLock' in navigator) {
      navigator.wakeLock.request('screen').catch(() => {});
    }
  });

  show(ui.start);
  if (demo) {
    // Offline demo: no PC, default thresholds.
    config = { exercises: [{ id: 'squat' }, { id: 'pushup' }, { id: 'situp' }], aspect: 16 / 9 };
    buildCounters();
    coach = new EU.Coach(config.exercises);
    ui.startMsg.textContent = T('demo');
    ui.startBtn.hidden = false;
  } else {
    connect();
  }
})();
