// Detector checks on synthetic poses. Shared by the Node runner
// (detectors.test.js, `npm test`) and the browser page (detectors.html),
// so they also run where Node isn't installed.

(function (root) {
  'use strict';

  // Thresholds in the range the calibration produces for a child; the gates
  // are the shipped config.ini values.
  const SQUAT = { standing: 165, down: 115, goingDown: 155, goingUp: 130,
                  minRepMs: 800, maxRepMs: 5000, qualityMin: 0, maxHipDrift: 0.12, minHipDrop: 0.05, mode: 0 };
  const PUSHUP = { up: 160, down: 110, goingDown: 150, goingUp: 125, plankMin: 150,
                   minRepMs: 800, maxRepMs: 5000, qualityMin: 0 };
  const FRAME_MS = 85;

  // Feed `ms` of synthetic frames through a Coach and tally what it says.
  function simulate(EU, poseAt, ms, exercises) {
    const coach = new EU.Coach(exercises || [{ id: 'squat', params: SQUAT }, { id: 'pushup', params: PUSHUP }]);
    const tally = { squat: 0, pushup: 0, drops: [], postures: new Set() };
    for (let t = 0; t <= ms; t += FRAME_MS) {
      const r = coach.update(poseAt(t), t);
      tally.postures.add(r.posture);
      for (const rep of r.reps) tally[rep.id]++;
      for (const d of r.drops) tally.drops.push(d.id + ':' + d.why);
    }
    return tally;
  }

  function cases(EU, S) {
    const squats = (period, top, bottom) => (t) => S.squatPose(S.cycleAngle(t, period, top, bottom));
    const pushups = (period, top, bottom) => (t) => S.pushupPose(S.cycleAngle(t, period, top, bottom));
    // 5 full cycles + a bit (each cycle = period + 600 ms rest).
    const five = (period) => 5 * (period + 600) + 300;

    return [
      ['angleAt: right angle', (eq) => {
        eq(Math.round(EU.angleAt({ x: 1, y: 0 }, { x: 0, y: 0 }, { x: 0, y: 1 })), 90);
      }],
      ['synthetic poses have the requested angles', (eq) => {
        const s = S.squatPose(100), p = S.pushupPose(100);
        eq(Math.round(EU.angleAt(s[11], s[13], s[15])), 100);
        eq(Math.round(EU.angleAt(p[5], p[7], p[9])), 100);
      }],
      ['five deep squats count five', (eq) => {
        const t = simulate(EU, squats(2000, 175, 88), five(2000));
        eq(t.squat, 5, 'squats ' + t.drops.join(','));
        eq(t.pushup, 0);
      }],
      ['five push-ups count five', (eq) => {
        const t = simulate(EU, pushups(1800, 172, 95), five(1800));
        eq(t.pushup, 5, 'push-ups ' + t.drops.join(','));
        eq(t.squat, 0);
      }],
      ['shallow squats do not count', (eq) => {
        const t = simulate(EU, squats(2000, 175, 140), five(2000));
        eq(t.squat, 0);
      }],
      ['half push-ups do not count', (eq) => {
        const t = simulate(EU, pushups(1800, 172, 135), five(1800));
        eq(t.pushup, 0);
      }],
      ['too fast squats are dropped for timing', (eq) => {
        const fast = { ...SQUAT, minRepMs: 1500 };
        const t = simulate(EU, squats(1000, 175, 88), five(1000), [{ id: 'squat', params: fast }]);
        eq(t.squat, 0);
        eq(t.drops.some((d) => d === 'squat:timing'), true, 'timing drop reported');
      }],
      ['standing still never counts', (eq) => {
        const t = simulate(EU, () => S.squatPose(176), 8000);
        eq(t.squat + t.pushup, 0);
        eq(t.postures.has(EU.Posture.Vertical), true, 'posture latched vertical');
      }],
      ['walking across the frame is not a squat', (eq) => {
        // Same knee motion, but the whole body slides 0.3 frame widths per rep.
        const walk = (t) => {
          const shift = ((t % 2600) / 2600) * 0.3;
          return S.squatPose(S.cycleAngle(t, 2000, 175, 88)).map((j) => ({ ...j, x: j.x + shift }));
        };
        const t = simulate(EU, walk, five(2000));
        eq(t.squat, 0);
        eq(t.drops.some((d) => d === 'squat:drift'), true, 'drift drop reported');
      }],
      ['unknown exercise ids are ignored', (eq) => {
        const coach = new EU.Coach([{ id: 'situp', params: {} }, { id: 'squat', params: SQUAT }]);
        eq(coach.items.length, 1);
      }],
    ];
  }

  const api = { cases };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.DetectorSpec = api;
})(typeof self !== 'undefined' ? self : this);
