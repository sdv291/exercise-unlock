// Exercise detection on the phone - a straight port of the service's C++
// detectors (ExerciseUnlockService/PostureClassifier.cpp, SquatDetector.cpp,
// PushUpDetector.cpp, AngleFilter.h), so a phone rep means the same thing
// as a webcam rep. The thresholds come from the PC (config.ini, set by the
// calibration) in the "config" message.
//
// Poses are 17 MoveNet / COCO joints {x, y, c}, already converted into the
// PC camera's coordinate convention (see toPcSpace in phone.js): x and y
// normalized to the frame, with the PC camera's aspect ratio. That is what
// the calibrated angles were measured in.
//
// Adding an exercise (sit-ups, ...): write a detector class with
// update(pose, nowMs) -> {counted, quality, durMs} | null, add it to
// EXERCISES with the posture it needs, and teach the service to accept its
// id in PhoneLink.cpp.

(function (root) {
  'use strict';

  const J = {
    nose: 0, lShoulder: 5, rShoulder: 6, lElbow: 7, rElbow: 8, lWrist: 9, rWrist: 10,
    lHip: 11, rHip: 12, lKnee: 13, rKnee: 14, lAnkle: 15, rAnkle: 16,
  };

  // Angle at b formed by b->a and b->c, degrees; 0 on degenerate input.
  function angleAt(a, b, c) {
    const ax = a.x - b.x, ay = a.y - b.y, cx = c.x - b.x, cy = c.y - b.y;
    const mag = Math.sqrt(ax * ax + ay * ay) * Math.sqrt(cx * cx + cy * cy);
    if (mag < 1e-6) return 0;
    const cos = Math.max(-1, Math.min(1, (ax * cx + ay * cy) / mag));
    return Math.acos(cos) * 180 / Math.PI;
  }

  // Rolling median with 2-sigma outlier rejection (AngleMedianFilter).
  class AngleMedianFilter {
    constructor(window = 5) { this.window = window; this.history = []; }
    reset() { this.history = []; }
    push(raw) {
      const h = this.history;
      if (h.length >= this.window) h.shift();
      h.push(raw);
      if (h.length < 3) return raw;
      const sorted = h.slice().sort((a, b) => a - b);
      const median = sorted[Math.floor(sorted.length / 2)];
      const mean = h.reduce((s, v) => s + v, 0) / h.length;
      const stddev = Math.sqrt(h.reduce((s, v) => s + (v - mean) * (v - mean), 0) / h.length);
      if (stddev < 0.5) return mean;
      const limit = 2 * stddev;
      let sum = 0, n = 0;
      for (const v of h) if (Math.abs(v - median) <= limit) { sum += v; n++; }
      return n > 0 ? sum / n : median;
    }
  }

  // ------------------------------------------------------------ posture

  const Posture = { None: 'none', Vertical: 'vertical', Horizontal: 'horizontal' };

  // PostureClassifier: "standing upright" (squats) vs "laid out across the
  // frame" (push-ups), latched with asymmetric hysteresis.
  class PostureClassifier {
    constructor() {
      this.confMin = 0.20;
      this.stableFrames = 6;
      this.unlatchFrames = 20;
      this.reset();
    }
    reset() { this.latched = Posture.None; this.pending = Posture.None; this.streak = 0; }

    update(pose) {
      const ok = (j) => j.c >= this.confMin;
      const avg = (idx) => {
        let x = 0, y = 0, n = 0;
        for (const i of idx) if (ok(pose[i])) { x += pose[i].x; y += pose[i].y; n++; }
        return n ? { x: x / n, y: y / n } : null;
      };
      const sh = avg([J.lShoulder, J.rShoulder]), hp = avg([J.lHip, J.rHip]);
      const kn = avg([J.lKnee, J.rKnee]), ak = avg([J.lAnkle, J.rAnkle]);
      const nose = ok(pose[J.nose]) ? pose[J.nose] : null;

      let raw = Posture.None;
      if (nose && sh && hp && ak) {
        const dxHead = Math.abs(nose.x - ak.x), dyHead = Math.abs(nose.y - ak.y);
        const bodyDy = Math.max(sh.y, hp.y, ak.y) - Math.min(sh.y, hp.y, ak.y);
        if (dxHead >= 0.15 && (dyHead <= 1e-3 || dxHead >= dyHead * 1.8) && bodyDy <= 0.20) {
          raw = Posture.Horizontal;
        }
      }
      if (raw === Posture.None && sh && hp && kn && ak) {
        const topY = nose ? nose.y : sh.y;
        const dy = ak.y - topY;
        const bodyDx = Math.max(sh.x, hp.x, kn.x, ak.x) - Math.min(sh.x, hp.x, kn.x, ak.x);
        const slack = 0.02;
        const stacked = hp.y > sh.y - slack && ak.y > hp.y - slack;
        if (dy >= 0.30 && (bodyDx <= 1e-3 || dy >= bodyDx * 1.5) && stacked) raw = Posture.Vertical;
      }

      this.raw = raw;                                // diagnostics only
      if (raw === this.pending) this.streak++;
      else { this.pending = raw; this.streak = 1; }
      if (this.pending !== this.latched) {
        const need = this.pending === Posture.None ? this.unlatchFrames : this.stableFrames;
        if (this.streak >= need) this.latched = this.pending;
      }
      return this.latched;
    }
  }

  // ------------------------------------------------------------ squat

  const Phase = { Unknown: 0, Top: 1, GoingDown: 2, Down: 3, GoingUp: 4, NotInPosition: 5 };

  // SquatDetector (knee angle, MAX of the confident legs, plus the
  // hip-drift / hip-drop / oscillation gates). Params mirror config.ini
  // [squat]: standing, down, goingDown, goingUp, minRepMs, maxRepMs,
  // qualityMin, maxHipDrift, minHipDrop, mode, standingHipFrac, downHipFrac,
  // goingDownHipFrac, goingUpHipFrac.
  class SquatDetector {
    constructor(p) {
      this.p = Object.assign({
        standing: 165, down: 120, goingDown: 155, goingUp: 135,
        minRepMs: 1000, maxRepMs: 6000, qualityMin: 0, maxHipDrift: 0, minHipDrop: 0,
        mode: 0, standingHipFrac: 0.45, downHipFrac: 0.25, goingDownHipFrac: 0.40, goingUpHipFrac: 0.30,
      }, p || {});
      this.confMin = 0.22;
      this.alpha = 0.70;
      this.reset();
    }
    reset() {
      this.phase = Phase.Unknown;
      this.angle = 0;
      this.smoothInit = false;
      this.miss = 0;
      this.filter = new AngleMedianFilter();
      this.hipHistory = [];
      this.clearRep();
    }
    clearRep() {
      this.repStart = 0; this.repMin = 999; this.startHipX = -1; this.startHipY = -1;
      this.maxHipY = -1; this.osc = 0;
    }

    update(pose, now) {
      const p = this.p, c = this.confMin;
      const ok3 = (a, b, d) => pose[a].c >= c && pose[b].c >= c && pose[d].c >= c;
      let l = 0, r = 0, nGood = 0;
      const lHave = ok3(J.lHip, J.lKnee, J.lAnkle), rHave = ok3(J.rHip, J.rKnee, J.rAnkle);
      if (lHave) { l = angleAt(pose[J.lHip], pose[J.lKnee], pose[J.lAnkle]); nGood++; }
      if (rHave) { r = angleAt(pose[J.rHip], pose[J.rKnee], pose[J.rAnkle]); nGood++; }
      this.legs = nGood;                             // diagnostics only
      if (nGood === 0) {
        if (++this.miss >= 20) { this.phase = Phase.Unknown; this.smoothInit = false; this.repStart = 0; }
        return null;
      }
      this.miss = 0;
      const rawMax = lHave && rHave ? Math.max(l, r) : (lHave ? l : r);
      const raw = this.filter.push(rawMax);

      let hipX = -1, hipY = -1;
      {
        let sx = 0, sy = 0, n = 0;
        for (const i of [J.lHip, J.rHip]) if (pose[i].c >= c) { sx += pose[i].x; sy += pose[i].y; n++; }
        if (n) { hipX = sx / n; hipY = sy / n; }
      }
      if (hipY > 0) {
        this.hipHistory.push(hipY);
        if (this.hipHistory.length > 12) this.hipHistory.shift();
      }
      if (this.repStart && hipY > 0 && (this.maxHipY < 0 || hipY > this.maxHipY)) this.maxHipY = hipY;

      this.angle = this.smoothInit ? this.alpha * raw + (1 - this.alpha) * this.angle : raw;
      this.smoothInit = true;
      const angle = this.angle;
      if (this.phase >= Phase.GoingDown && this.phase <= Phase.GoingUp && angle < this.repMin) this.repMin = angle;

      // Floor-relative hip height for mode 1 / 2.
      let hipFrac = 0.5, haveFloor = false;
      if (p.mode !== 0) {
        const y = (i) => (pose[i].c >= c ? pose[i].y : -1);
        let shY = -1, hpY = -1, akY = -1;
        if (y(J.lShoulder) >= 0 && y(J.lHip) >= 0 && y(J.lAnkle) >= 0) {
          shY = y(J.lShoulder); hpY = y(J.lHip); akY = y(J.lAnkle);
        }
        if (y(J.rShoulder) >= 0 && y(J.rHip) >= 0 && y(J.rAnkle) >= 0) {
          if (shY < 0) { shY = y(J.rShoulder); hpY = y(J.rHip); akY = y(J.rAnkle); }
          else { shY = (shY + y(J.rShoulder)) / 2; hpY = (hpY + y(J.rHip)) / 2; akY = (akY + y(J.rAnkle)) / 2; }
        }
        if (shY > 0 && akY > shY + 0.05) {
          hipFrac = Math.max(0, Math.min(1.2, (akY - hpY) / (akY - shY)));
          haveFloor = true;
        }
      }
      const pick = (angleOk, floorOk, both) =>
        p.mode === 0 ? angleOk : p.mode === 1 ? floorOk : (both === 'and' ? angleOk && floorOk : angleOk || floorOk);
      const isStanding = () => pick(angle >= p.standing, haveFloor && hipFrac >= p.standingHipFrac, 'and');
      const isDown = () => nGood >= 2 && pick(angle <= p.down, haveFloor && hipFrac <= p.downHipFrac, 'and');
      const crossDown = () => pick(angle < p.goingDown, haveFloor && hipFrac < p.goingDownHipFrac, 'or');
      const crossUp = () => pick(angle > p.goingUp, haveFloor && hipFrac > p.goingUpHipFrac, 'or');

      if (this.phase === Phase.Unknown) {
        if (isStanding()) this.phase = Phase.Top;
        else if (isDown() || angle < p.goingUp) {
          this.phase = Phase.Down; this.repStart = now; this.startHipX = hipX;
        } else this.phase = Phase.Top;
        return null;
      }

      switch (this.phase) {
        case Phase.Top:
          if (crossDown()) {
            this.phase = Phase.GoingDown;
            this.repStart = now;
            this.startHipX = hipX;
            // Highest hip of the last second = real standing level (the
            // smoothed angle notices the descent a few frames late).
            let base = hipY;
            for (const v of this.hipHistory) if (v > 0 && (base < 0 || v < base)) base = v;
            this.startHipY = base;
            this.maxHipY = hipY;
            this.osc = 0;
          }
          break;
        case Phase.GoingDown:
          if (isDown()) this.phase = Phase.Down;
          else if (isStanding()) { this.phase = Phase.Top; this.clearRep(); }
          break;
        case Phase.Down:
          if (crossUp()) this.phase = Phase.GoingUp;
          break;
        case Phase.GoingUp: {
          if (isDown()) { this.phase = Phase.Down; this.osc++; break; }
          if (!isStanding()) break;
          const dur = this.repStart ? now - this.repStart : 0;
          const span = p.standing - p.down;
          const quality = Math.max(0, Math.min(1, span > 1 ? (p.standing - this.repMin) / span : 1));
          const timingOk = this.repStart !== 0 && dur >= p.minRepMs && dur <= p.maxRepMs;
          const qualityOk = p.qualityMin <= 0 || quality >= p.qualityMin;
          const driftOk = !(p.maxHipDrift > 0 && this.startHipX >= 0 && hipX >= 0) ||
                          Math.abs(hipX - this.startHipX) <= p.maxHipDrift;
          const dropOk = !(p.minHipDrop > 0 && this.startHipY >= 0 && this.maxHipY >= 0) ||
                         this.maxHipY - this.startHipY >= p.minHipDrop;
          const counted = timingOk && qualityOk && driftOk && dropOk && this.osc === 0;
          const result = { counted, quality, durMs: dur,
            why: counted ? '' : !timingOk ? 'timing' : !qualityOk ? 'depth' : !driftOk ? 'drift' : !dropOk ? 'hip' : 'shaky' };
          this.phase = Phase.Top;
          this.clearRep();
          return result;
        }
      }
      return null;
    }
  }

  // ------------------------------------------------------------ push-up

  // PushUpDetector (elbow angle averaged over confident arms, plank gate,
  // wrists near ankle height). Params mirror config.ini [pushup]: up, down,
  // goingDown, goingUp, plankMin, minRepMs, maxRepMs, qualityMin.
  class PushUpDetector {
    constructor(p) {
      this.p = Object.assign({
        up: 160, down: 110, goingDown: 150, goingUp: 125, plankMin: 140,
        minRepMs: 800, maxRepMs: 6000, qualityMin: 0,
      }, p || {});
      this.confMin = 0.18;
      this.alpha = 0.70;
      this.reset();
    }
    reset() {
      this.phase = Phase.Unknown;
      this.angle = 0; this.plank = 0;
      this.smoothInit = false; this.miss = 0;
      this.filter = new AngleMedianFilter();
      this.repStart = 0; this.repMin = 999;
    }

    update(pose, now) {
      const p = this.p, c = this.confMin;
      const ok3 = (a, b, d) => pose[a].c >= c && pose[b].c >= c && pose[d].c >= c;
      let sum = 0, n = 0;
      if (ok3(J.lShoulder, J.lElbow, J.lWrist)) { sum += angleAt(pose[J.lShoulder], pose[J.lElbow], pose[J.lWrist]); n++; }
      if (ok3(J.rShoulder, J.rElbow, J.rWrist)) { sum += angleAt(pose[J.rShoulder], pose[J.rElbow], pose[J.rWrist]); n++; }

      let plank = 0, havePlank = false;
      if (ok3(J.lShoulder, J.lHip, J.lAnkle)) { plank = angleAt(pose[J.lShoulder], pose[J.lHip], pose[J.lAnkle]); havePlank = true; }
      else if (ok3(J.rShoulder, J.rHip, J.rAnkle)) { plank = angleAt(pose[J.rShoulder], pose[J.rHip], pose[J.rAnkle]); havePlank = true; }

      // Hands on the floor: wrists about as low as the ankles.
      const wy = (s, w) => (pose[w].c >= c ? pose[w].y : pose[s].c >= c ? pose[s].y : -1);
      const ay = (a) => (pose[a].c >= c ? pose[a].y : -1);
      const wL = wy(J.lShoulder, J.lWrist), wR = wy(J.rShoulder, J.rWrist);
      const aL = ay(J.lAnkle), aR = ay(J.rAnkle);
      const wrist = wL >= 0 && wR >= 0 ? (wL + wR) / 2 : (wL >= 0 ? wL : wR);
      const ankle = aL >= 0 && aR >= 0 ? (aL + aR) / 2 : (aL >= 0 ? aL : aR);
      const handsDown = wrist >= 0 && ankle >= 0 && Math.abs(wrist - ankle) < 0.25;
      this.gate = { arms: n, plank: havePlank, handsDown };   // diagnostics only

      if (n === 0 || !havePlank || !handsDown) {
        if (++this.miss >= 20) { this.phase = Phase.Unknown; this.smoothInit = false; this.repStart = 0; }
        return null;
      }
      this.miss = 0;
      const raw = this.filter.push(sum / n);
      if (!this.smoothInit) { this.angle = raw; this.plank = plank; this.smoothInit = true; }
      else {
        this.angle = this.alpha * raw + (1 - this.alpha) * this.angle;
        this.plank = this.alpha * plank + (1 - this.alpha) * this.plank;
      }
      const elbow = this.angle;
      if (this.phase >= Phase.GoingDown && this.phase <= Phase.GoingUp && elbow < this.repMin) this.repMin = elbow;

      if (this.plank < p.plankMin) { this.phase = Phase.NotInPosition; return null; }

      if (this.phase === Phase.Unknown || this.phase === Phase.NotInPosition) {
        if (elbow >= p.up) this.phase = Phase.Top;
        else if (elbow <= p.down || elbow < p.goingUp) { this.phase = Phase.Down; this.repStart = now; }
        else this.phase = Phase.Top;
        return null;
      }

      switch (this.phase) {
        case Phase.Top:
          if (elbow < p.goingDown) { this.phase = Phase.GoingDown; this.repStart = now; }
          break;
        case Phase.GoingDown:
          if (elbow <= p.down) this.phase = Phase.Down;
          else if (elbow >= p.up) { this.phase = Phase.Top; this.repStart = 0; }
          break;
        case Phase.Down:
          if (elbow > p.goingUp) this.phase = Phase.GoingUp;
          break;
        case Phase.GoingUp: {
          if (elbow <= p.down) { this.phase = Phase.Down; break; }
          if (elbow < p.up) break;
          const dur = this.repStart ? now - this.repStart : 0;
          const span = p.up - p.down;
          const quality = Math.max(0, Math.min(1, span > 1 ? (p.up - this.repMin) / span : 1));
          const timingOk = dur >= p.minRepMs && dur <= p.maxRepMs;
          const qualityOk = p.qualityMin <= 0 || quality >= p.qualityMin;
          const counted = timingOk && qualityOk;
          this.phase = Phase.Top; this.repStart = 0; this.repMin = 999;
          return { counted, quality, durMs: dur, why: counted ? '' : !timingOk ? 'timing' : 'depth' };
        }
      }
      return null;
    }
  }

  // ------------------------------------------------------------ registry

  // id -> how to detect it. `posture` gates the detector exactly like the
  // service does (no squats counted while lying down and vice versa).
  const EXERCISES = {
    squat: { posture: Posture.Vertical, create: (params) => new SquatDetector(params) },
    pushup: { posture: Posture.Horizontal, create: (params) => new PushUpDetector(params) },
  };

  // Runs the posture classifier and every enabled exercise on one pose.
  class Coach {
    // exercises: [{id, params}] from the PC's config message.
    constructor(exercises) {
      this.posture = new PostureClassifier();
      this.items = [];
      for (const e of exercises || []) {
        const def = EXERCISES[e.id];
        if (def) this.items.push({ id: e.id, posture: def.posture, detector: def.create(e.params) });
      }
    }
    // -> {posture, active: id|null, angle, reps: [{id, quality, durMs}], drops: [...]}
    update(pose, now) {
      const posture = this.posture.update(pose);
      const out = { posture, active: null, angle: 0, reps: [], drops: [] };
      for (const it of this.items) {
        if (it.posture !== posture) continue;   // same gate as the service
        const r = it.detector.update(pose, now);
        out.active = it.id;
        out.angle = it.detector.angle;
        if (r && r.counted) out.reps.push({ id: it.id, quality: r.quality, durMs: r.durMs });
        else if (r) out.drops.push({ id: it.id, why: r.why, durMs: r.durMs });
      }
      return out;
    }
  }

  const api = { J, angleAt, AngleMedianFilter, Posture, PostureClassifier, Phase, SquatDetector, PushUpDetector, EXERCISES, Coach };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.EU = api;
})(typeof self !== 'undefined' ? self : this);
