// Compile a Jarscript 3 script with the sandbox's own compiler
// (apps/jar/tools/jarsandbox/jarscript3.js), for the prompt lab's server.
//   stdin:  {"script": "...", "frames": ["f1", "f2"]}   (or a list of them)
//   stdout: {"ok": true, "bytes", "ops", "states", "handlers", "vars", "traits"}
//           {"ok": false, "error": "...", "line": N}
'use strict';
const path = require('path');
const J = require(path.join(__dirname, '..', 'jarsandbox', 'jarscript3.js'));

function one(r){
  try {
    const p = J.compile(String(r.script || ''), (r.frames && r.frames.length) ? r.frames.map(String) : ['f1']);
    return {ok: true, bytes: p.bytes, ops: p.ops, states: p.states.map(s => s.name),
      handlers: p.allHandlers.map(h => (h.state >= 0 ? p.states[h.state].name + '/' : '') + h.ev + (h.arg ? ' ' + h.arg : '') + (h.every ? ' ' + h.every + 's' : '')),
      vars: p.vars.map(v => v.name), traits: p.traits};
  } catch (e){
    return {ok: false, error: e.message, line: e.line || 0};
  }
}

let input = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', d => input += d);
process.stdin.on('end', () => {
  let req;
  try { req = JSON.parse(input); } catch (e){ process.stdout.write(JSON.stringify({ok: false, error: 'bad request: ' + e.message, line: 0})); return; }
  process.stdout.write(JSON.stringify(Array.isArray(req) ? req.map(one) : one(req)));
});
