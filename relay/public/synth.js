// Synthetic side-view skeletons for a given knee / elbow angle, in the same
// coordinate convention the detectors use. Drives the page's demo mode
// (?demo=squat / ?demo=pushup, no camera needed) and the unit tests.

(function (root) {
  'use strict';

  const DEG = Math.PI / 180;

  function blank() {
    const pose = [];
    for (let i = 0; i < 17; i++) pose.push({ x: 0, y: 0, c: 0.9 });
    return pose;
  }
  // Same point for the left and right joint (profile view), right side a
  // hair behind so the skeleton doesn't collapse into one line.
  function both(pose, l, r, x, y) {
    pose[l] = { x, y, c: 0.9 };
    pose[r] = { x: x - 0.004, y: y - 0.002, c: 0.9 };
  }

  // Horizontal distances in the PC camera's convention are divided by its
  // aspect ratio (x is normalized to the wider side).
  const ASPECT = 16 / 9;

  // Standing at 180, deep squat about 90. Facing right. The knee angle is
  // exact in the detectors' coordinates; the thigh keeps a real length of
  // 0.2 frame heights, so the hip moves back as far as in a real picture.
  function squatPose(kneeDeg) {
    const t = kneeDeg * DEG;
    const ankle = { x: 0.5, y: 0.9 };
    const knee = { x: 0.5, y: 0.7 };
    // knee->hip makes the knee angle with knee->ankle (straight down).
    const a = 0.2 / Math.hypot(ASPECT * Math.sin(t), Math.cos(t));
    const hip = { x: knee.x - a * Math.sin(t), y: knee.y + a * Math.cos(t) };
    const lean = (180 - kneeDeg) / 90;
    const shoulder = { x: hip.x + 0.05 * lean, y: hip.y - 0.25 };
    const pose = blank();
    pose[0] = { x: shoulder.x + 0.03, y: shoulder.y - 0.08, c: 0.9 };
    for (const k of [1, 2, 3, 4]) pose[k] = { x: shoulder.x + 0.02, y: shoulder.y - 0.09, c: 0.9 };
    both(pose, 5, 6, shoulder.x, shoulder.y);
    both(pose, 7, 8, shoulder.x + 0.06, shoulder.y + 0.05);
    both(pose, 9, 10, shoulder.x + 0.12, shoulder.y + 0.05);
    both(pose, 11, 12, hip.x, hip.y);
    both(pose, 13, 14, knee.x, knee.y);
    both(pose, 15, 16, ankle.x, ankle.y);
    return pose;
  }

  // Arms straight at 180, bottom about 90. Lying across the frame, head right.
  function pushupPose(elbowDeg) {
    const half = (elbowDeg * DEG) / 2;
    const L = 0.07;                                   // upper arm = forearm
    const wrist = { x: 0.75, y: 0.9 };
    const d = 2 * L * Math.sin(half);                 // shoulder-wrist distance
    const shoulder = { x: 0.75, y: wrist.y - d };
    const elbow = { x: 0.75 - L * Math.cos(half), y: (shoulder.y + wrist.y) / 2 };
    const ankle = { x: 0.15, y: 0.88 };
    const hip = { x: 0.45, y: (shoulder.y + ankle.y) / 2 };
    const knee = { x: 0.30, y: (hip.y + ankle.y) / 2 };
    const pose = blank();
    pose[0] = { x: 0.85, y: shoulder.y - 0.01, c: 0.9 };
    for (const k of [1, 2, 3, 4]) pose[k] = { x: 0.84, y: shoulder.y - 0.02, c: 0.9 };
    both(pose, 5, 6, shoulder.x, shoulder.y);
    both(pose, 7, 8, elbow.x, elbow.y);
    both(pose, 9, 10, wrist.x, wrist.y);
    both(pose, 11, 12, hip.x, hip.y);
    both(pose, 13, 14, knee.x, knee.y);
    both(pose, 15, 16, ankle.x, ankle.y);
    return pose;
  }

  // One rep every periodMs: top -> bottom -> top (cosine), then a short rest.
  function cycleAngle(nowMs, periodMs, top, bottom) {
    const phase = (nowMs % (periodMs + 600)) / periodMs;
    if (phase >= 1) return top;
    return bottom + (top - bottom) * (0.5 + 0.5 * Math.cos(phase * 2 * Math.PI));
  }

  const api = { squatPose, pushupPose, cycleAngle };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.EUSynth = api;
})(typeof self !== 'undefined' ? self : this);
