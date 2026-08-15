'use strict';

const common = require('../common');
const assert = require('assert');
const net = require('net');

const before = net.createServer().listen(0);
const cp = process.quiesce.checkpoint();
assert.strictEqual(typeof cp, 'bigint');

const after = net.createServer().listen(0);
const timer = setTimeout(() => {}, 100000);
const interval = setInterval(() => {}, 100000);
const immediate = setImmediate(() => {});

const report = process.quiesce.report(cp);
assert.deepStrictEqual(report.resources.map((r) => r.type), ['TCPServerWrap']);
assert.strictEqual(typeof report.resources[0].asyncId, 'number');
assert.strictEqual(report.timers.timeouts.length, 2);
assert.strictEqual(report.timers.immediates.length, 1);

const clean = process.quiesce.report(process.quiesce.checkpoint());
assert.deepStrictEqual(clean.resources, []);
assert.deepStrictEqual(clean.timers, { timeouts: [], immediates: [] });

before.close(common.mustCall());
after.close(common.mustCall());
clearTimeout(timer);
clearInterval(interval);
clearImmediate(immediate);
