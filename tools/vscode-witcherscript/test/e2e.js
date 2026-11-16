// The adapter against TW3SE's real debugger.lua (test\fakegame.lua stands in for the game), as VS Code drives it.
// node test\e2e.js [path to lua.exe]   (default: ..\..\build\lua.exe, made by build\make_lua_exe.bat)
'use strict';
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { Session, scriptKey } = require('../adapter');

const REPO = path.resolve(__dirname, '..', '..', '..');
const LUA = process.argv[2] || path.join(REPO, 'build', 'lua.exe');

let fails = 0;
function expect(cond, what, extra) {
  if (cond) console.log('ok   ' + what);
  else { fails++; console.log('FAIL ' + what + (extra !== undefined ? '\n  ' + JSON.stringify(extra) : '')); }
}

// a game folder with one mod script
const game = fs.mkdtempSync(path.join(os.tmpdir(), 'tw3se-dbg-'));
const modScript = path.join(game, 'mods', 'modTest', 'content', 'scripts', 'game', 'player', 'r4player.ws');
fs.mkdirSync(path.dirname(modScript), { recursive: true });
fs.writeFileSync(modScript, '// test\n');
const luaMod = path.join(game, 'bin', 'x64_dx12', 'tw3se', 'mods', 'demo', 'init.lua');
fs.mkdirSync(path.dirname(luaMod), { recursive: true });
fs.writeFileSync(luaMod, [
  'local function add(a, b)',
  '  local s = a + b',
  '  return s * 2',
  'end',
  'local up = 10',
  'function DEMO(x)',
  '  local y = add(x, up)',
  '  return y',
  'end',
  ''].join('\n'));

let child = null;
const server = net.createServer((sock) => {
  child = spawn(LUA, [path.join(__dirname, 'fakegame.lua'), REPO.replace(/\\/g, '/'), game]);
  child.stdout.pipe(sock);
  sock.pipe(child.stdin);
  child.stderr.on('data', (d) => process.stdout.write('  game: ' + d));
});

server.listen(0, '127.0.0.1', async () => {
  const port = server.address().port;
  const s = new Session({ port }, []);
  const waiting = [];
  const events = [];
  s.onDidSendMessage((m) => {
    if (m.type === 'event') events.push(m);
    for (const w of waiting.slice()) {
      if (w.match(m)) { waiting.splice(waiting.indexOf(w), 1); w.resolve(m); }
    }
  });
  let seq = 1;
  const wait = (match, ms = 3000) => new Promise((resolve, reject) => {
    const old = events.find(match);
    if (old) { events.splice(events.indexOf(old), 1); resolve(old); return; }
    const w = { match, resolve };
    waiting.push(w);
    setTimeout(() => reject(new Error('timeout')), ms);
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

  try {
    expect(scriptKey('C:/W3/Mods/modX/content/scripts/game/player/r4player.ws') === 'game\\player\\r4player.ws',
      'script key from a path');
    const init = await request('initialize', { adapterID: 'witcherscript' });
    expect(init.success && init.body.supportsConditionalBreakpoints, 'initialize');
    const att = await request('attach', {});
    expect(att.success, 'attach', att.message);
    await event('initialized');
    const bp = await request('setBreakpoints', {
      source: { path: 'C:\\work\\content\\scripts\\game\\player\\r4player.ws' },
      breakpoints: [{ line: 12 }, { line: 14, condition: 'x > 5' }, { line: 15, logMessage: 'x={x}' }],
    });
    expect(bp.success && bp.body.breakpoints.length === 3 && bp.body.breakpoints.every((b) => b.verified),
      'three breakpoints verified', bp.body);
    await request('configurationDone');

    child.stdin.write('@hit 12\n');
    const st = await event('stopped');
    expect(st.body.reason === 'breakpoint' && st.body.threadId === 1, 'stopped at the breakpoint');
    const th = await request('threads');
    expect(th.body.threads.length === 1, 'one thread');
    const stack = await request('stackTrace', { threadId: 1 });
    const f0 = stack.body.stackFrames[0];
    expect(f0 && f0.line === 12 && f0.source.path === modScript, 'the frame opens the mod\'s file', f0);
    expect(stack.body.stackFrames[1].name.endsWith('(native)'), 'a native frame is marked');
    const sc = await request('scopes', { frameId: 1 });
    expect(sc.body.scopes.map((x) => x.name).join(',') === 'Locals,self', 'scopes', sc.body);
    const vars = await request('variables', { variablesReference: sc.body.scopes[0].variablesReference });
    expect(vars.body.variables[0].name === 'x' && vars.body.variables[0].value === '3', 'x is 3', vars.body);
    const ev = await request('evaluate', { expression: 'x * 2', frameId: 1, context: 'watch' });
    expect(ev.success && ev.body.result === '6', 'evaluate x * 2', ev.body);
    const bad = await request('evaluate', { expression: 'nope(', frameId: 1 });
    expect(!bad.success && /expression/.test(bad.message), 'a broken expression answers with the error', bad.message);
    const sv = await request('setVariable', { variablesReference: sc.body.scopes[0].variablesReference, name: 'x', value: '42' });
    expect(sv.success && sv.body.value === '42', 'setVariable x = 42', sv.body);
    const nx = await request('next', { threadId: 1 });
    expect(nx.success, 'next');
    await event('continued');

    child.stdin.write('@step 13\n');
    const st2 = await event('stopped');
    expect(st2.body.reason === 'step', 'stopped after the step');
    await request('continue', { threadId: 1 });

    child.stdin.write('@hit 15\n');
    const out = await event('output');
    expect(out.body.output === 'x=42\n', 'log point output', out.body);

    child.stdin.write('@hit 14\n');  // x is 42 now: the condition holds
    const st3 = await event('stopped');
    expect(st3.body.reason === 'breakpoint', 'conditional breakpoint stops');
    await request('continue', { threadId: 1 });

    // a Lua breakpoint, in a copy of the mod's file elsewhere (the game matches the path below tw3se)
    const lbp = await request('setBreakpoints', {
      source: { path: 'C:\\work\\repo\\tw3se\\mods\\demo\\init.lua' }, breakpoints: [{ line: 2 }],
    });
    expect(lbp.success && lbp.body.breakpoints[0].verified, 'a Lua breakpoint');
    child.stdin.write('@luacall\n');
    const ls = await event('stopped');
    expect(ls.body.reason === 'breakpoint', 'stopped in the Lua mod');
    const lstack = await request('stackTrace', { threadId: 1 });
    const l0 = lstack.body.stackFrames[0];
    expect(l0.name === 'add' && l0.line === 2 && l0.source.path === luaMod, 'the frame opens the mod\'s Lua file', l0);
    const lsc = await request('scopes', { frameId: 1 });
    const lv = await request('variables', { variablesReference: lsc.body.scopes[0].variablesReference });
    expect(lv.body.variables.map((v) => v.name + '=' + v.value).join(',') === 'a=3,b=10', 'a = 3, b = 10', lv.body);
    await request('continue', { threadId: 1 });

    const dc = await request('disconnect', {});
    expect(dc.success, 'disconnect');
  } catch (e) {
    fails++;
    console.log('FAIL ' + e.message);
  }
  if (child) child.kill();
  server.close();
  fs.rmSync(game, { recursive: true, force: true });
  console.log(fails === 0 ? 'ALL OK' : fails + ' FAILED');
  process.exit(fails === 0 ? 0 : 1);
});
