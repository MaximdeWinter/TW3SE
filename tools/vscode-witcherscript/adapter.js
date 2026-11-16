// The debug adapter: VS Code's Debug Adapter Protocol on one side, TW3SE's debugger (lua\lib\debugger.lua, a TCP
// connection to 127.0.0.1) on the other. Runs inside VS Code (extension.js) and in the tests without it.
'use strict';
const net = require('net');
const fs = require('fs');
const path = require('path');

const THREAD = 1;

function escapeArg(s) {
  return String(s).replace(/\\/g, '\\\\').replace(/\t/g, '\\t').replace(/\n/g, '\\n');
}

// "C:\...\content\scripts\game\player\r4player.ws" -> "game\player\r4player.ws" (the path the game knows a script by);
// a Lua file: "lua:" and its path below tw3se ("lua:mods\mymod\init.lua"), the game matches the end of the path
function scriptKey(file) {
  const p = file.replace(/\//g, '\\').toLowerCase();
  if (p.endsWith('.lua')) {
    const t = p.lastIndexOf('\\tw3se\\');
    return 'lua:' + (t >= 0 ? p.slice(t + 7) : path.win32.basename(p));
  }
  const at = p.lastIndexOf('\\scripts\\');
  return at >= 0 ? p.slice(at + 9) : path.win32.basename(p);
}

class Session {
  constructor(config, workspaceFolders) {
    this.config = config || {};
    this.folders = workspaceFolders || [];
    this.listeners = [];
    this.seq = 1;
    this.gameSeq = 1;
    this.pending = new Map();  // game seq -> callback
    this.buf = '';
    this.socket = null;
    this.game = '';
    this.sources = new Map();  // script key -> path on disk or null
    // VS Code's vscode.Event: a function taking the listener
    this.onDidSendMessage = (listener) => {
      this.listeners.push(listener);
      return { dispose: () => { this.listeners = this.listeners.filter((l) => l !== listener); } };
    };
  }

  dispose() {
    if (this.socket) this.socket.destroy();
  }

  emit(msg) {
    msg.seq = this.seq++;
    for (const l of this.listeners) l(msg);
  }
  event(name, body) { this.emit({ type: 'event', event: name, body: body || {} }); }
  respond(req, body, error) {
    const msg = { type: 'response', request_seq: req.seq, command: req.command, success: !error, body: body || {} };
    if (error) msg.message = error;
    this.emit(msg);
  }

  // one command to the game; the callback gets (error, body)
  call(name, args, cb) {
    if (!this.socket) { cb('not connected to the game'); return; }
    const seq = this.gameSeq++;
    this.pending.set(seq, cb);
    this.socket.write([seq, name].concat(args || []).map(escapeArg).join('\t') + '\n');
  }

  fromGame(line) {
    let m;
    try { m = JSON.parse(line); } catch (e) { return; }
    if (m.event === 'stopped') {
      this.event('stopped', { reason: m.reason || 'breakpoint', threadId: THREAD, allThreadsStopped: true });
    } else if (m.event === 'continued') {
      this.event('continued', { threadId: THREAD, allThreadsContinued: true });
    } else if (m.event === 'output') {
      this.event('output', { category: m.category || 'console', output: m.text || '' });
    } else if (m.seq !== undefined && this.pending.has(m.seq)) {
      const cb = this.pending.get(m.seq);
      this.pending.delete(m.seq);
      cb(m.ok ? null : (m.error || 'error'), m.body || {});
    }
  }

  connect(req) {
    const port = this.config.port || 29170;
    const host = this.config.host || '127.0.0.1';
    const s = net.createConnection({ port, host });
    let connected = false;
    s.setEncoding('utf8');
    s.on('connect', () => {
      connected = true;
      this.socket = s;
      this.call('hello', [], (err, body) => {
        if (err) { this.respond(req, {}, err); return; }
        this.game = body.game || '';
        this.respond(req);
        this.event('initialized');
      });
    });
    s.on('data', (d) => {
      this.buf += d;
      let at;
      while ((at = this.buf.indexOf('\n')) >= 0) {
        const line = this.buf.slice(0, at).replace(/\r$/, '');
        this.buf = this.buf.slice(at + 1);
        this.fromGame(line);
      }
    });
    s.on('error', (e) => {
      if (!connected) {
        this.respond(req, {}, `No game on ${host}:${port}. Start The Witcher 3 with "debugger = on" in ` +
          `bin\\x64_dx12\\tw3se\\tw3se.config (${e.code || e.message})`);
      }
    });
    s.on('close', () => {
      this.socket = null;
      for (const cb of this.pending.values()) cb('the game closed the connection');
      this.pending.clear();
      if (connected) this.event('terminated');
    });
  }

  // a script the game names ("game\player\r4player.ws") as a file on disk: the folders from the launch
  // configuration, the workspace, the game's mods, its DLC and its own scripts, in that order
  resolve(key) {
    if (!key) return null;
    if (this.sources.has(key)) return this.sources.get(key);
    const roots = [];
    for (const r of [].concat(this.config.scriptRoots || [], this.folders)) {
      roots.push(r, path.join(r, 'scripts'), path.join(r, 'content', 'scripts'));
    }
    if (this.game) {
      for (const group of ['mods', 'dlc']) {
        const dir = path.join(this.game, group);
        let names = [];
        try { names = fs.readdirSync(dir).sort(); } catch (e) { /* none */ }
        for (const n of names) roots.push(path.join(dir, n, 'content', 'scripts'));
      }
      roots.push(path.join(this.game, 'content', 'content0', 'scripts'));
    }
    let found = null;
    for (const r of roots) {
      const p = path.join(r, key);
      if (fs.existsSync(p)) { found = p; break; }
    }
    // the game knows some mod scripts by a shorter path (local\x.ws as x.ws): a file below a root ending with it
    const tail = '\\' + key.toLowerCase();
    const walk = (dir, depth) => {
      let items = [];
      try { items = fs.readdirSync(dir, { withFileTypes: true }); } catch (e) { return null; }
      for (const it of items) {
        const p = path.join(dir, it.name);
        if (it.isDirectory()) {
          if (depth > 0) { const r = walk(p, depth - 1); if (r) return r; }
        } else if (p.toLowerCase().endsWith(tail)) {
          return p;
        }
      }
      return null;
    };
    for (const r of roots) {
      if (found) break;
      if (fs.existsSync(r)) found = walk(r, 8);
    }
    this.sources.set(key, found);
    return found;
  }

  source(key) {
    const p = this.resolve(key);
    return p ? { name: path.basename(p), path: p } : { name: path.win32.basename(key) || key };
  }

  handleMessage(req) {
    if (req.type !== 'request') return;
    const a = req.arguments || {};
    const relay = (name, args, shape) => this.call(name, args, (err, body) =>
      err ? this.respond(req, {}, err) : this.respond(req, shape ? shape(body) : {}));
    switch (req.command) {
      case 'initialize':
        this.respond(req, {
          supportsConfigurationDoneRequest: true,
          supportsConditionalBreakpoints: true,
          supportsLogPoints: true,
          supportsEvaluateForHovers: true,
          supportsSetVariable: true,
        });
        break;
      case 'attach':
      case 'launch':
        this.connect(req);
        break;
      case 'configurationDone':
        this.respond(req);
        break;
      case 'setBreakpoints': {
        const file = (a.source && a.source.path) || '';
        const bps = a.breakpoints || (a.lines || []).map((line) => ({ line }));
        const specs = bps.map((b) => b.logMessage ? `${b.line}!${b.logMessage}`
          : b.condition ? `${b.line}?${b.condition}` : `${b.line}`);
        relay('breakpoints', [scriptKey(file)].concat(specs), (body) => ({
          breakpoints: (body.breakpoints || []).map((b) => ({ verified: !!b.verified, line: b.line })),
        }));
        break;
      }
      case 'threads':
        this.respond(req, { threads: [{ id: THREAD, name: 'WitcherScript' }] });
        break;
      case 'stackTrace':
        relay('stack', [], (body) => {
          const frames = (body.frames || []).map((f) => f.path
            ? { id: f.id, name: f.name, line: f.line || 0, column: 1,
              source: { name: path.win32.basename(f.path), path: f.path } }
            : f.file
            ? { id: f.id, name: f.name, line: f.line || 0, column: 1, source: this.source(f.file) }
            : { id: f.id, name: f.name + ' (native)', line: 0, column: 0, presentationHint: 'subtle' });
          return { stackFrames: frames, totalFrames: frames.length };
        });
        break;
      case 'scopes':
        relay('scopes', [a.frameId || 1], (body) => ({
          scopes: (body.scopes || []).map((s) => ({ name: s.name, variablesReference: s.ref, expensive: false })),
        }));
        break;
      case 'variables':
        relay('variables', [a.variablesReference], (body) => ({
          variables: (body.variables || []).map((v) => ({
            name: v.name, value: v.value, type: v.type, variablesReference: v.ref || 0,
          })),
        }));
        break;
      case 'evaluate':
        relay('evaluate', [a.frameId || 1, a.expression], (body) => ({
          result: body.value, type: body.type, variablesReference: body.ref || 0,
        }));
        break;
      case 'setVariable':
        relay('setVariable', [a.variablesReference, a.name, a.value], (body) => ({
          value: body.value, type: body.type, variablesReference: body.ref || 0,
        }));
        break;
      case 'continue':
        relay('continue', [], () => ({ allThreadsContinued: true }));
        break;
      case 'next':
      case 'stepIn':
      case 'stepOut':
      case 'pause':
        relay(req.command, []);
        break;
      case 'disconnect':
      case 'terminate':
        if (this.socket) {
          this.call('disconnect', [], () => {
            if (this.socket) this.socket.end();
            this.respond(req);
          });
        } else {
          this.respond(req);
        }
        break;
      default:
        this.respond(req, {}, `${req.command} is not supported`);
    }
  }
}

module.exports = { Session, scriptKey };
