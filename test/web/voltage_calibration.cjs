// Exercise the real page controls with DOM/API stubs, without needing a device.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../components/assets/web_file/status.html'), 'utf8');
const code = html.slice(html.indexOf('    const voltageControlIds'), html.indexOf('    function currentRaw()'));
assert(code.includes('function saveVoltage'));
const nodes = new Map();
const $ = id => {
  if (!nodes.has(id)) nodes.set(id, { value: '', disabled: false, textContent: '' });
  return nodes.get(id);
};
const requests = [];
let saved = 0;
let fail = false;
const context = vm.createContext({ $, document: { activeElement: null },
  calibParams: null, fmt: (v, n) => Number(v).toFixed(n),
  markCalibSaved: (node, msg) => { saved++; node.textContent = msg; },
  load: async () => {},
  fetch: async (url, opts) => {
    requests.push({ url, body: JSON.parse(opts.body) });
    return { ok: !fail, json: async () => fail ? { ok: false, reason: 'persist_failed' } : { ok: true, voltage_k: 2.1 } };
  }
});
vm.runInContext(code, context);
const render = (frontend, available = true) => {
  context.calibParams = { voltage: { frontend, available, editable: frontend === 226 && available,
    active_k: frontend === 228 ? 1 : 2, stored_ina226_k: 2.1, uncalibrated_v: 2 } };
  context.renderVoltageControls();
};
const tick = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  render(226);
  assert.equal($('calibVoltageK').value, 2.1);
  assert.equal($('calibVoltageSave').disabled, false);
  assert.match($('calibVoltageInfo').textContent, /生效电压 K=2.000000.*已保存电压 K=2.100000/);
  $('calibVoltageK').value = '2.123456';
  $('calibVoltageSave').onclick();
  assert.equal($('calibVoltageSave').disabled, true);
  await tick();
  assert.deepEqual(requests.at(-1), { url: '/api/calibration/voltage', body: { voltage_k_ppm: 2123456 } });
  $('calibVoltageReal').value = '4.185';
  $('calibVoltageMeasured').onclick();
  await tick();
  assert.deepEqual(requests.at(-1).body, { real_voltage_mv: 4185 });
  $('calibVoltageReset').onclick();
  await tick();
  assert.deepEqual(requests.at(-1).body, { reset: true });
  const beforeInvalid = requests.length;
  for (const value of ['', 'NaN', '0.499999', '4.000001']) {
    $('calibVoltageK').value = value;
    $('calibVoltageSave').onclick();
  }
  $('calibVoltageReal').value = '0';
  $('calibVoltageMeasured').onclick();
  assert.equal(requests.length, beforeInvalid);
  render(228);
  assert.equal($('calibVoltageK').value, '1');
  assert.equal($('calibVoltageSave').disabled, true);
  await context.saveVoltage({ voltage_k_ppm: 2000000 });
  assert.equal(requests.length, beforeInvalid);
  render(226, false);
  assert.equal($('calibVoltageMeasured').disabled, true);
  await context.saveVoltage({ real_voltage_mv: 4185 });
  assert.equal(requests.length, beforeInvalid);
  render(226);
  fail = true;
  const beforeFail = saved;
  await context.saveVoltage({ reset: true });
  assert.equal(saved, beforeFail);
  assert.match($('calibVoltageMsg').textContent, /persist_failed/);
  assert.equal($('calibVoltageSave').disabled, false);
  assert(requests.every(r => !('base_k' in r.body) && !('real_current_ma' in r.body)));
  console.log('PASS: voltage controls, units, independent payloads, fixed INA228, unavailable samples, validation, API failure');
})().catch(e => { console.error(e); process.exitCode = 1; });
