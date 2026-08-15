'use strict';

const common = require('../common');
const assert = require('assert');

const older = setTimeout(common.mustCall(), 20);
const cp = process.quiesce.checkpoint();
setTimeout(common.mustNotCall(), 20);

process.quiesce.quiesce(cp);

assert.strictEqual(process.quiesce.report(cp).timers.timeouts.length, 0);
assert.ok(older.hasRef());
