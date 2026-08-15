'use strict';

const common = require('../common');
const assert = require('assert');
const net = require('net');

const survivor = net.createServer().listen(0, common.mustCall(() => {
  const cp = process.quiesce.checkpoint();

  net.createServer().listen(0);
  setInterval(common.mustNotCall(), 10);
  setTimeout(common.mustNotCall(), 10);
  setImmediate(common.mustNotCall());

  process.quiesce.quiesce(cp);

  setTimeout(common.mustCall(() => {
    const report = process.quiesce.report(cp);
    assert.deepStrictEqual(report.resources, []);
    assert.deepStrictEqual(report.timers.timeouts, []);
    assert.deepStrictEqual(report.timers.immediates, []);

    net.connect(survivor.address().port, common.mustCall(function() {
      this.destroy();
      survivor.close(common.mustCall());
    }));
  }), 50);
}));
