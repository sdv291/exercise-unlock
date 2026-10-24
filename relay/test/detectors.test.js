'use strict';
// node --test test/
const test = require('node:test');
const assert = require('node:assert');
const EU = require('../public/exercises.js');
const S = require('../public/synth.js');
const { cases } = require('./detectors.spec.js');

for (const [name, fn] of cases(EU, S)) {
  test(name, () => fn((actual, expected, msg) => assert.strictEqual(actual, expected, msg)));
}
