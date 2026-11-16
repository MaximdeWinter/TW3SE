// The adapter against the running game (debugger = on in tw3se.config, the test mod modTw3seTest installed):
// a breakpoint in TW3SE_StackOuter (local\tw3se_test_core.ws:137), the call started through the pipe.
// node test\live.js
'use strict';
const path = require('path');
const { spawn } = require('child_process');
const { Session } = require('../adapter');

const REPO = path.resolve(__dirname, '..', '..', '..');
let fails = 0;
function expect(cond, what, extra) {
  if (cond) console.log('ok   ' + what);
  else { fails++; console.log('FAIL ' + what + (extra !== undefined ? '\n  ' + JSON.stringify(extra) : '')); }
}

const s = new Session({ port: 29170 }, []);
const waiting = [];
const events = [];
s.onDidSendMessage((m) => {
  if (m.type === 'event') events.push(m);
  for (const w of waiting.slice()) if (w.match(m)) { waiting.splice(waiting.indexOf(w), 1); w.resolve(m); }
});
let seq = 1;
const wait = (match, ms = 15000) => new Promise((resolve, reject) => {
  const old = events.find(match);
  if (old) { events.splice(events.indexOf(old), 1); resolve(old); return; }
  waiting.push({ match, resolve });
  setTimeout(() => reject(new Error('timeout waiting')), ms);
});
const request = (command, args) => {
  const req = { type: 'request', seq: seq++, command, arguments: args || {} };
  const p = wait((m) => m.type === 'response' && m.request_seq === req.seq);
  s.handleMessage(req);
  return p;
};
const event = (name) => wait((m) => m.type === 'event' && m.event === name).then((m) => {
  const i = events.indexOf(m);
  if (i >= 0) events.splice(i, 1);
  return m;
});
const lua = (code) => new Promise((resolve) => {
  const py = spawn('python', [path.join(REPO, 'tools', 'tw3se.py'), 'lua', code], { cwd: REPO });
  let out = '';
  py.stdout.on('data', (d) => { out += d; });
  py.stderr.on('data', (d) => { out += d; });
  py.on('close', () => resolve(out.trim()));
});

(async () => {
  try {
    await request('initialize', {});
    const att = await request('attach', {});
    expect(att.success, 'attach to the game', att.message);
    if (!att.success) throw new Error('no game');
    await event('initialized');
    const bp = await request('setBreakpoints', {
      source: { path: path.join(REPO, 'tests', 'modTw3seTest', 'content', 'scripts', 'local', 'tw3se_test_core.ws') },
      breakpoints: [{ line: 137 }],
    });
    expect(bp.success && bp.body.breakpoints[0].verified, 'breakpoint on line 137');
    await request('configurationDone');

    const call = lua('return w3.call("TW3SE_StackOuter", 1)');  // stands at the breakpoint until we go on
    const st = await event('stopped');
    expect(st.body.reason === 'breakpoint', 'the game stands at line 137');
    const stack = await request('stackTrace', { threadId: 1 });
    const f0 = stack.body.stackFrames[0];
    expect(f0 && f0.name === 'TW3SE_StackOuter' && f0.line === 137, 'top frame TW3SE_StackOuter:137', f0);
    const sc = await request('scopes', { frameId: 1 });
    const vars = await request('variables', { variablesReference: sc.body.scopes[0].variablesReference });
    const byName = Object.fromEntries(vars.body.variables.map((v) => [v.name, v]));
    expect(byName.n && byName.n.value === '1' && byName.r && byName.r.value === '2', 'n = 1, r = 2', vars.body);
    const ev = await request('evaluate', { expression: 'n + r', frameId: 1 });
    expect(ev.body.result === '3', 'evaluate n + r', ev.body);
    const pl = await request('evaluate', { expression: 'w3.player()', frameId: 1 });
    expect(pl.success && pl.body.variablesReference > 0, 'an object can be opened', pl.body);
    if (pl.body.variablesReference) {
      const fields = await request('variables', { variablesReference: pl.body.variablesReference });
      expect(fields.success && fields.body.variables.length > 10, 'the player\'s fields', fields.body.variables.length);
    }
    const sv = await request('setVariable', { variablesReference: sc.body.scopes[0].variablesReference, name: 'r', value: '10' });
    expect(sv.success, 'r = 10', sv.message);
    await request('stepIn', { threadId: 1 });
    const st2 = await event('stopped');
    const stack2 = await request('stackTrace', { threadId: 1 });
    expect(st2.body.reason === 'step' && stack2.body.stackFrames[0].name === 'TW3SE_StackInner',
      'step in: TW3SE_StackInner', stack2.body.stackFrames.slice(0, 2));
    await request('stepOut', { threadId: 1 });
    const st3 = await event('stopped');
    const stack3 = await request('stackTrace', { threadId: 1 });
    expect(st3.body.reason === 'step' && stack3.body.stackFrames[0].name === 'TW3SE_StackOuter',
      'step out: back in TW3SE_StackOuter', stack3.body.stackFrames[0]);
    await request('continue', { threadId: 1 });
    const result = await call;
    expect(/11/.test(result), 'the call returns 11 (r was changed to 10)', result);
    // Lua: a file in tw3se\lua, required and called through the pipe
    const tw = 'Q:\\SteamLibrary\\steamapps\\common\\The Witcher 3\\bin\\x64_dx12\\tw3se';
    const demo = path.join(tw, 'lua', 'tw3se_dbg_demo.lua');
    require('fs').writeFileSync(demo, 'local function add(a, b)\n  local s = a + b\n  return s * 2\nend\n' +
      'local up = 10\nfunction DEMO(x)\n  local y = add(x, up)\n  return y\nend\n');
    const lbp = await request('setBreakpoints', { source: { path: demo }, breakpoints: [{ line: 2 }] });
    expect(lbp.success, 'a Lua breakpoint');
    const lcall = lua('package.loaded.tw3se_dbg_demo = nil require("tw3se_dbg_demo") return DEMO(3)');
    const ls = await event('stopped');
    const lstack = await request('stackTrace', { threadId: 1 });
    expect(ls.body.reason === 'breakpoint' && lstack.body.stackFrames[0].name === 'add', 'the game stands in the Lua file',
      lstack.body.stackFrames.slice(0, 2));
    const lsc = await request('scopes', { frameId: 1 });
    await request('setVariable', { variablesReference: lsc.body.scopes[0].variablesReference, name: 'a', value: '5' });
    await request('continue', { threadId: 1 });
    expect(/30/.test(await lcall), 'DEMO(3) returns 30 (a was set to 5)');
    await request('setBreakpoints', { source: { path: demo }, breakpoints: [] });
    require('fs').unlinkSync(demo);
    await request('disconnect', {});
  } catch (e) {
    fails++;
    console.log('FAIL ' + e.message);
  }
  console.log(fails === 0 ? 'ALL OK' : fails + ' FAILED');
  process.exit(fails === 0 ? 0 : 1);
})();
