/* Jarscript 3 (a draft): the language -- lexer, parser and compiler.

   Shared by three things, so they cannot disagree about what a script means:
   - apps/jar/tools/jarsandbox/index.html, the sandbox (loaded with <script src>, which
     also works when the page is opened straight from disk);
   - apps/jar/tools/promptlab/index.html, the prompt lab's page;
   - apps/jar/tools/promptlab/js3check.js, which checks Claude's scripts under node.
   The words a script may use are documented in jarscript3.md beside this
   file, which is also what the prompt lab hands Claude. Change both together.

   No DOM and no world here: compile(src, frameNames) -> a program, or a CErr
   with the line. The machine that runs a program is in index.html. */
'use strict';
const MAXVARS = 8;
const EVENTS = ['poke','tick','bump','hit','land','signal','near','every'];
const PARTICLES = ['leaf','dust','feather','spark','heart','note'];
const PROPS = ['x','y','vx','vy','angle','spin','gravity','bounce','friction'];
const SENSES = new Set([...PROPS,'speed','grounded','onperch','perched','facing','other','me','statetime','time','soil','jarw','jarh','impact','blocked','state','frame','physics']);
const CONSTS = {left:-1,right:1,true:1,false:0,none:0,on:1,off:0};
const FUNCS = {random:[2,2],chance:[1,1],pick:[1,8],abs:[1,1],min:[2,2],max:[2,2],sign:[1,1],clamp:[3,3],
  sin:[1,1],cos:[1,1],sqrt:[1,1],round:[1,1],floor:[1,1],dist:[1,2],angleto:[1,1],find:[1,2],count:[1,1],has:[2,2]};
const FIELDS = ['x','y','vx','vy','w','h','id','speed'];
const KEYWORDS = new Set(['traits','var','anim','perch','solid','body','layer','scale','state','on','every','end','if','then','elif','else','while','do','repeat','times','loop','wait','until','go','frame','play','once','stop','physics','roll','say','log','emit','sway','shake','face','toward','move','to','walk','hop','speed','arc','signal','return','within','and','or','not']);

/* ============================================================
   Lexer
   ============================================================ */
class CErr extends Error { constructor(msg, line){ super(msg); this.line = line; } }
function lex(src){
  const toks = [], lines = src.split('\n');
  for (let ln = 0; ln < lines.length; ln++){
    const s = lines[ln], L = ln + 1; let i = 0;
    while (i < s.length){
      const c = s[i];
      if (c === ' ' || c === '\t' || c === '\r'){ i++; continue; }
      if (c === '#') break;
      if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(s[i+1] || ''))){
        let j = i; while (j < s.length && /[0-9.]/.test(s[j])) j++;
        const txt = s.slice(i, j);
        if ((txt.match(/\./g) || []).length > 1) throw new CErr(`'${txt}' is not a number`, L);
        let v = parseFloat(txt);
        if (s.startsWith('ms', j) && !/\w/.test(s[j+2] || '')){ v /= 1000; j += 2; }
        else if (s[j] === 's' && !/\w/.test(s[j+1] || '')) j += 1;
        else if (/[A-Za-z_]/.test(s[j] || '')) throw new CErr(`'${s.slice(i).match(/^[\w.]+/)[0]}' — a number can only end in s or ms`, L);
        toks.push({t:'num', v, line:L}); i = j; continue;
      }
      if (/[A-Za-z_]/.test(c)){
        let j = i; while (j < s.length && /\w/.test(s[j])) j++;
        toks.push({t:'id', v:s.slice(i, j), line:L}); i = j; continue;
      }
      if (c === '"'){
        let j = i + 1; while (j < s.length && s[j] !== '"') j++;
        if (j >= s.length) throw new CErr('this text has no closing "', L);
        toks.push({t:'str', v:s.slice(i+1, j), line:L}); i = j + 1; continue;
      }
      const two = s.slice(i, i+2);
      if (['==','!=','<=','>=','+=','-=','*=','/='].includes(two)){ toks.push({t:'op', v:two, line:L}); i += 2; continue; }
      if ('+-*/%<>=(),:;.@'.includes(c)){ toks.push({t:'op', v:c, line:L}); i++; continue; }
      throw new CErr(`unexpected character '${c}'`, L);
    }
    toks.push({t:'nl', line:L});
  }
  toks.push({t:'eof', line:lines.length});
  return toks;
}

/* ============================================================
   Parser: source -> tree
   ============================================================ */
function parse(src){
  const T = lex(src); let p = 0;
  const is = (t, v) => T[p].t === t && (v === undefined || T[p].v === v);
  const isId = v => is('id', v), isOp = v => is('op', v);
  const desc = k => k.t === 'nl' ? 'the end of the line' : k.t === 'eof' ? 'the end of the script' : k.t === 'str' ? `"${k.v}"` : `'${k.v}'`;
  const fail = (msg, k = T[p]) => { throw new CErr(msg, k.line); };
  function expect(t, v, what){ if (!is(t, v)) fail(`expected ${what || (v ? `'${v}'` : t)}, found ${desc(T[p])}`); return T[p++]; }
  function skipSep(){ while (is('nl') || isOp(';')) p++; }
  function atEnd(){ return is('nl') || isOp(';') || is('eof') || isId('end') || isId('else') || isId('elif'); }
  function endStmt(){
    if (is('nl') || isOp(';')){ p++; return; }
    if (atEnd()) return;
    fail(`unexpected ${desc(T[p])} — the statement should end here`);
  }
  function numLit(){
    let neg = false; if (isOp('-')){ neg = true; p++; }
    const k = expect('num', undefined, 'a number'); return neg ? -k.v : k.v;
  }
  const prog = {traits:[], vars:[], anims:{}, perches:[], solid:null, body:null, layer:1, scale:1, states:[], handlers:[]};

  // ---- expressions
  function parseExpr(){ return parseOr(); }
  function parseOr(){ let l = parseAnd(); while (isId('or')){ const line = T[p++].line; l = {k:'bin', op:'or', l, r:parseAnd(), line}; } return l; }
  function parseAnd(){ let l = parseNot(); while (isId('and')){ const line = T[p++].line; l = {k:'bin', op:'and', l, r:parseNot(), line}; } return l; }
  function parseNot(){ if (isId('not')){ const line = T[p++].line; return {k:'not', e:parseNot(), line}; } return parseCmp(); }
  function parseCmp(){
    let l = parseAdd();
    if (is('op') && ['==','!=','<','<=','>','>='].includes(T[p].v)){ const k = T[p++]; l = {k:'bin', op:k.v, l, r:parseAdd(), line:k.line}; }
    else if (isOp('=')) fail(`'=' sets a value; to compare, use '=='`);
    return l;
  }
  function parseAdd(){ let l = parseMul(); while (isOp('+') || isOp('-')){ const k = T[p++]; l = {k:'bin', op:k.v, l, r:parseMul(), line:k.line}; } return l; }
  function parseMul(){ let l = parseUnary(); while (isOp('*') || isOp('/') || isOp('%')){ const k = T[p++]; l = {k:'bin', op:k.v, l, r:parseUnary(), line:k.line}; } return l; }
  function parseUnary(){ if (isOp('-')){ const line = T[p++].line; return {k:'neg', e:parseUnary(), line}; } return parsePost(); }
  function parsePost(){
    let e = parsePrim();
    while (isOp('.')){ p++; const f = expect('id', undefined, 'a field name after "."'); e = {k:'field', e, f:f.v, line:f.line}; }
    return e;
  }
  function parsePrim(){
    const k = T[p];
    if (k.t === 'num'){ p++; return {k:'num', v:k.v, line:k.line}; }
    if (k.t === 'str'){ p++; return {k:'str', v:k.v, line:k.line}; }
    if (isOp('(')){ p++; const e = parseExpr(); expect('op', ')', "')'"); return e; }
    if (k.t === 'id'){
      if (['end','then','else','elif','do'].includes(k.v)) fail(`expected a value, found ${desc(k)}`);
      p++;
      if (isOp('(')){
        p++; const args = [];
        if (!isOp(')')){ args.push(parseExpr()); while (isOp(',')){ p++; args.push(parseExpr()); } }
        expect('op', ')', `')' to close ${k.v}(`);
        return {k:'call', name:k.v, args, line:k.line};
      }
      return {k:'id', v:k.v, line:k.line};
    }
    fail(`expected a value, found ${desc(k)}`);
  }

  // ---- statements
  function thenTok(){ if (isId('then') || isOp(':')) p++; else if (!is('nl')) fail(`expected 'then' or ':' after the condition, found ${desc(T[p])}`); }
  function opt(){ return atEnd() ? null : parseExpr(); }
  function closeEnd(what, line){ if (!isId('end')) fail(`expected 'end' to close the ${what} from line ${line}, found ${desc(T[p])}`); p++; endStmt(); }
  function parseBlock(openLine, what, st){
    const out = [];
    for (;;){
      skipSep();
      if (is('eof')) throw new CErr(`the ${what} that starts on line ${openLine} has no 'end' — or an if/loop inside it is missing its own`, openLine);
      if (isId('end') || isId('else') || isId('elif')) break;
      if (st && (isId('on') || isId('every'))){ st.handlers.push(parseHandler()); continue; }
      if (st && isId('state')) fail(`state '${st.name}' (line ${openLine}) needs an 'end' before the next state`);
      out.push(parseStmt());
    }
    return out;
  }
  function parseHandler(){
    const k = T[p++]; let ev, arg = null, within = 24, every = null;
    if (k.v === 'every'){ ev = 'every'; every = numLit(); }
    else {
      const e = expect('id', undefined, 'an event name'); ev = e.v;
      if (!EVENTS.includes(ev)) fail(`'${ev}' is not an event — events are ${EVENTS.join(', ')}`, e);
      if (ev === 'every') every = numLit();
      if (ev === 'signal') arg = expect('id', undefined, 'the signal name, e.g. on signal landed:').v;
      if (ev === 'near'){ arg = expect('id', undefined, 'a trait, e.g. on near bird:').v; if (isId('within')){ p++; within = numLit(); } }
    }
    if (every !== null && every <= 0) fail('every needs a time above zero', k);
    if (isOp(':')) p++;
    const body = parseBlock(k.line, `'on ${ev}' handler`);
    closeEnd(`'on ${ev}' handler`, k.line);
    return {ev, arg, within, every, body, line:k.line};
  }
  function parseStmt(){
    const k = T[p], line = k.line;
    if (k.t !== 'id') fail(`a statement can't start with ${desc(k)}`);
    const w = k.v, nx = T[p+1];
    if (nx.t === 'op' && ['=','+=','-=','*=','/='].includes(nx.v)){ p += 2; const e = parseExpr(); endStmt(); return {k:'assign', name:w, op:nx.v, e, line}; }
    p++;
    let s;
    switch (w){
      case 'if': {
        const arms = []; let c = parseExpr(); thenTok(); arms.push({c, b:parseBlock(line, 'if')}); let els = null;
        for (;;){
          if (isId('elif')){ p++; c = parseExpr(); thenTok(); arms.push({c, b:parseBlock(line, 'if')}); }
          else if (isId('else')){ p++; if (isOp(':')) p++; els = parseBlock(line, 'if'); break; }
          else break;
        }
        closeEnd('if', line); return {k:'if', arms, els, line};
      }
      case 'while': { const c = parseExpr(); if (isOp(':') || isId('do')) p++; const b = parseBlock(line, 'while'); closeEnd('while', line); return {k:'while', c, b, line}; }
      case 'repeat': { const n = parseExpr(); if (isId('times')) p++; if (isOp(':')) p++; const b = parseBlock(line, 'repeat'); closeEnd('repeat', line); return {k:'repeat', n, b, line}; }
      case 'loop': { if (isOp(':')) p++; const b = parseBlock(line, 'loop'); closeEnd('loop', line); return {k:'loop', b, line}; }
      case 'wait':
        if (isId('until')){ p++; s = {k:'waituntil', c:parseExpr(), line}; }
        else s = {k:'wait', e:opt(), line};
        break;
      case 'go': s = {k:'go', name:expect('id', undefined, 'a state name after go').v, line}; break;
      case 'frame': s = {k:'frame', e:parseExpr(), line}; break;
      case 'play': { const n = expect('id', undefined, 'an anim or frame name').v; let once = false; if (isId('once')){ p++; once = true; } else if (isId('loop')) p++; s = {k:'play', name:n, once, line}; break; }
      case 'stop': s = {k:'stop', line}; break;
      case 'physics': case 'roll': {
        const v = expect('id', undefined, "'on' or 'off'").v;
        if (!['on','off','true','false'].includes(v)) fail(`${w} takes 'on' or 'off', not '${v}'`);
        s = {k:'flag', name:w, v:(v === 'on' || v === 'true') ? 1 : 0, line}; break;
      }
      case 'say': s = {k:'say', e:parseExpr(), d:opt(), line}; break;
      case 'log': { const es = [parseExpr()]; while (isOp(',')){ p++; es.push(parseExpr()); } s = {k:'log', es, line}; break; }
      case 'emit': { const kind = expect('id', undefined, 'a particle kind').v; if (!PARTICLES.includes(kind)) fail(`'${kind}' is not a particle — try ${PARTICLES.join(', ')}`); s = {k:'emit', kind, n:opt(), line}; break; }
      case 'sway': s = {k:'sway', a:parseExpr(), d:opt(), line}; break;
      case 'shake': s = {k:'shake', d:opt(), line}; break;
      case 'face': if (isId('toward')){ p++; s = {k:'toward', e:parseExpr(), line}; } else s = {k:'face', e:parseExpr(), line}; break;
      case 'move': {
        expect('id', 'to', "'to' after move");
        let tgt;
        if (isId('perch')){ p++; let trait = null; if (is('id') && !['speed','arc'].includes(T[p].v)) trait = T[p++].v; tgt = {k:'perch', trait}; }
        else if (isId('ground')){ p++; tgt = {k:'ground'}; }
        else { const a = parseExpr(); if (isOp(',')){ p++; tgt = {k:'xy', a, b:parseExpr()}; } else tgt = {k:'item', a}; }
        let speed = null, arc = null;
        while (isId('speed') || isId('arc')){ const o = T[p++].v; if (o === 'speed') speed = parseExpr(); else arc = parseExpr(); }
        s = {k:'move', tgt, speed, arc, line}; break;
      }
      case 'walk': { const dx = parseExpr(); let speed = null; if (isId('speed')){ p++; speed = parseExpr(); } s = {k:'walk', dx, speed, line}; break; }
      case 'hop': { const dx = opt(); const h = dx ? opt() : null; s = {k:'hop', dx, h, line}; break; }
      case 'signal': { const n = expect('id', undefined, 'a signal name').v; let to = null; if (isId('to')){ p++; to = parseExpr(); } s = {k:'signal', name:n, to, line}; break; }
      case 'return': s = {k:'return', line}; break;
      case 'gravity': case 'bounce': case 'friction': case 'spin':
        s = {k:'assign', name:w, op:'=', e:parseExpr(), line}; break;
      default:
        if (w === 'state') fail(`'state' only starts a new state at the top level — is an 'end' missing above?`, k);
        if (w === 'on') fail(`handlers ('on ...') go at the top level or directly inside a state, not inside another block`, k);
        fail(`'${w}' is not a statement` + (SENSES.has(w) || prog.vars.some(v => v.name === w) ? ` — did you mean '${w} = ...'?` : ''), k);
    }
    endStmt(); return s;
  }

  // ---- top level
  for (;;){
    skipSep(); if (is('eof')) break;
    const k = T[p];
    if (k.t !== 'id') fail(`expected traits, var, anim, perch, state or on at the top level, found ${desc(k)}`);
    switch (k.v){
      case 'traits': p++; while (!atEnd()){ if (isOp(',')){ p++; continue; } prog.traits.push(expect('id', undefined, 'a trait word').v); } endStmt(); break;
      case 'var': p++;
        for (;;){ const n = expect('id', undefined, 'a variable name'); let init = 0; if (isOp('=')){ p++; init = numLit(); } prog.vars.push({name:n.v, init, line:n.line}); if (isOp(',')){ p++; continue; } break; }
        endStmt(); break;
      case 'anim': {
        p++; const n = expect('id', undefined, 'an anim name'); expect('op', '=', "'=' then frame names");
        const fr = []; while (!atEnd() && !isOp('@')){ if (isOp(',')){ p++; continue; } const f = T[p++]; if (f.t !== 'id' && f.t !== 'num') fail(`expected a frame name, found ${desc(f)}`, f); fr.push({v:f.v, line:f.line}); }
        let fps = 8; if (isOp('@')){ p++; fps = numLit(); if (isId('fps')) p++; }
        if (!fr.length) fail(`anim '${n.v}' lists no frames`, n);
        prog.anims[n.v] = {frames:fr, fps, line:n.line}; endStmt(); break;
      }
      case 'perch': { p++; const x = numLit(); expect('op', ',', "',' between x and y"); const y = numLit(); prog.perches.push({x, y, line:k.line}); endStmt(); break; }
      case 'solid': { p++; const a = [numLit()]; for (let i = 0; i < 3; i++){ expect('op', ',', "solid x, y, w, h"); a.push(numLit()); } prog.solid = {x:a[0], y:a[1], w:a[2], h:a[3]}; endStmt(); break; }
      case 'body': p++; prog.body = numLit(); endStmt(); break;
      case 'layer': p++; prog.layer = numLit(); endStmt(); break;
      case 'scale': p++; prog.scale = numLit(); if (prog.scale !== 1 && prog.scale !== 2) fail('scale is 1 or 2', k); endStmt(); break;
      case 'state': {
        p++; const n = expect('id', undefined, 'a state name'); if (isOp(':')) p++;
        const st = {name:n.v, line:n.line, body:[], handlers:[]};
        st.body = parseBlock(n.line, `state '${n.v}'`, st);
        closeEnd(`state '${n.v}'`, n.line); prog.states.push(st); break;
      }
      case 'on': case 'every': prog.handlers.push(parseHandler()); break;
      default: fail(`'${k.v}' can't start a line at the top level — expected traits, var, anim, perch, solid, body, layer, scale, state or on`);
    }
  }
  return prog;
}

/* ============================================================
   Compiler: tree -> flat stack code (one array per handler / state body)
   ============================================================ */
function compile(src, frameNames){
  const prog = parse(src);
  const C = {stateIdx:{}, varIdx:{}, frames:frameNames};
  if (prog.vars.length > MAXVARS) throw new CErr(`${prog.vars.length} variables — an item keeps at most ${MAXVARS}`, prog.vars[MAXVARS].line);
  prog.vars.forEach((v, i) => {
    if (C.varIdx[v.name] !== undefined) throw new CErr(`variable '${v.name}' is declared twice`, v.line);
    if (SENSES.has(v.name) || KEYWORDS.has(v.name) || FUNCS[v.name] || CONSTS[v.name] !== undefined) throw new CErr(`'${v.name}' is already a word in the language — pick another name`, v.line);
    C.varIdx[v.name] = i;
  });
  if (!prog.states.length) prog.states.push({name:'main', line:1, body:[], handlers:[]});
  prog.states.forEach((s, i) => { if (C.stateIdx[s.name] !== undefined) throw new CErr(`state '${s.name}' is declared twice`, s.line); C.stateIdx[s.name] = i; });
  for (const [n, a] of Object.entries(prog.anims)){
    a.idx = a.frames.map(f => {
      if (typeof f.v === 'number'){ if (f.v < 1 || f.v > frameNames.length) throw new CErr(`anim '${n}': there is no frame ${f.v}`, f.line); return f.v - 1; }
      const i = frameNames.indexOf(f.v); if (i < 0) throw new CErr(`anim '${n}': no frame called '${f.v}' (frames: ${frameNames.join(', ')})`, f.line); return i;
    });
    if (a.fps <= 0 || a.fps > 30) throw new CErr(`anim '${n}': fps should be 1..30`, a.line);
  }

  function gen(stmts, thread){
    const code = [];
    const em = (o, a, b, line) => { code.push({o, a, b, line}); return code.length - 1; };
    function ex(e){
      switch (e.k){
        case 'num': em('num', e.v, null, e.line); break;
        case 'str': em('str', e.v, null, e.line); break;
        case 'id':
          if (C.stateIdx[e.v] !== undefined) em('num', C.stateIdx[e.v], 'state ' + e.v, e.line);
          else if (C.varIdx[e.v] !== undefined) em('ldv', C.varIdx[e.v], e.v, e.line);
          else if (CONSTS[e.v] !== undefined) em('num', CONSTS[e.v], e.v, e.line);
          else if (SENSES.has(e.v)) em('lds', e.v, null, e.line);
          else if (frameNames.includes(e.v)) throw new CErr(`'${e.v}' is a frame — use it after 'frame' or 'play'`, e.line);
          else throw new CErr(`unknown name '${e.v}' — not a variable, sense or state (declare it with 'var ${e.v}')`, e.line);
          break;
        case 'bin': ex(e.l); ex(e.r); em('bin', e.op, null, e.line); break;
        case 'neg': ex(e.e); em('neg', null, null, e.line); break;
        case 'not': ex(e.e); em('not', null, null, e.line); break;
        case 'field':
          if (!FIELDS.includes(e.f)) throw new CErr(`'.${e.f}' is not a field — items have ${FIELDS.map(f => '.' + f).join(' ')}`, e.line);
          ex(e.e); em('field', e.f, null, e.line); break;
        case 'call': {
          const sig = FUNCS[e.name];
          if (!sig) throw new CErr(`'${e.name}()' is not a function — try ${Object.keys(FUNCS).join(', ')}`, e.line);
          if (e.args.length < sig[0] || e.args.length > sig[1]) throw new CErr(`${e.name}() takes ${sig[0] === sig[1] ? sig[0] : sig[0] + ' to ' + sig[1]} value${sig[1] > 1 ? 's' : ''}, got ${e.args.length}`, e.line);
          const traitAt = {find:0, count:0, has:1}[e.name];
          e.args.forEach((a, i) => {
            if (i === traitAt && a.k === 'id' && C.varIdx[a.v] === undefined) em('str', a.v, 'trait', a.line);
            else ex(a);
          });
          em('call', e.name, e.args.length, e.line); break;
        }
        default: throw new CErr('cannot compile this expression', e.line);
      }
    }
    const needThread = (s, what) => { if (!thread) throw new CErr(`'${what}' takes time, so it only works in a state's body — a handler must finish within one tick. Put it in a state and 'go' there.`, s.line); };
    function st(s){
      switch (s.k){
        case 'assign': {
          const isVar = C.varIdx[s.name] !== undefined;
          if (!isVar && !PROPS.includes(s.name)){
            if (SENSES.has(s.name)) throw new CErr(`'${s.name}' can be read but not set`, s.line);
            throw new CErr(`unknown variable '${s.name}' — declare it at the top with 'var ${s.name}'`, s.line);
          }
          if (s.op !== '='){ if (isVar) em('ldv', C.varIdx[s.name], s.name, s.line); else em('lds', s.name, null, s.line); }
          ex(s.e);
          if (s.op !== '=') em('bin', s.op[0], null, s.line);
          if (isVar) em('stv', C.varIdx[s.name], s.name, s.line); else em('stp', s.name, null, s.line);
          break;
        }
        case 'if': {
          const ends = [];
          for (const arm of s.arms){ ex(arm.c); const jz = em('jz', 0, null, s.line); arm.b.forEach(st); ends.push(em('jmp', 0, null, s.line)); code[jz].a = code.length; }
          if (s.els) s.els.forEach(st);
          ends.forEach(j => code[j].a = code.length); break;
        }
        case 'while': { const top = code.length; ex(s.c); const jz = em('jz', 0, null, s.line); s.b.forEach(st); em('jmp', top, null, s.line); code[jz].a = code.length; break; }
        case 'repeat': { ex(s.n); const top = em('rpt', 0, null, s.line); s.b.forEach(st); em('jmp', top, null, s.line); code[top].a = code.length; break; }
        case 'loop': { const top = code.length; s.b.forEach(st); em('jmp', top, null, s.line); break; }
        case 'wait': needThread(s, 'wait'); if (s.e) ex(s.e); else em('num', 0, null, s.line); em('wait', null, null, s.line); break;
        case 'waituntil': { needThread(s, 'wait until'); const top = code.length; ex(s.c); em('jzy', top, null, s.line); break; }
        case 'go': if (C.stateIdx[s.name] === undefined) throw new CErr(`there is no state '${s.name}' (states: ${prog.states.map(x => x.name).join(', ')})`, s.line); em('go', C.stateIdx[s.name], s.name, s.line); break;
        case 'frame':
          if (s.e.k === 'id' && frameNames.includes(s.e.v)) em('framec', frameNames.indexOf(s.e.v), s.e.v, s.line);
          else if (s.e.k === 'id' && C.varIdx[s.e.v] === undefined && !SENSES.has(s.e.v)) throw new CErr(`no frame called '${s.e.v}' (frames: ${frameNames.join(', ')})`, s.line);
          else { ex(s.e); em('framee', null, null, s.line); }
          break;
        case 'play': {
          if (prog.anims[s.name]) em('play', s.name, s.once ? 1 : 0, s.line);
          else if (frameNames.includes(s.name)) em('framec', frameNames.indexOf(s.name), s.name, s.line);
          else throw new CErr(`no anim or frame called '${s.name}' — declare it with 'anim ${s.name} = frame1 frame2 @ 8'`, s.line);
          if (s.once && thread && prog.anims[s.name]) em('await', null, 'anim', s.line);
          break;
        }
        case 'stop': em('stop', null, null, s.line); break;
        case 'flag': em('flag', s.name, s.v, s.line); break;
        case 'say': ex(s.e); if (s.d) ex(s.d); else em('num', 2, null, s.line); em('say', null, null, s.line); break;
        case 'log': s.es.forEach(ex); em('log', s.es.length, null, s.line); break;
        case 'emit': if (s.n) ex(s.n); else em('num', 1, null, s.line); em('emit', s.kind, null, s.line); break;
        case 'sway': ex(s.a); if (s.d) ex(s.d); else em('num', 1, null, s.line); em('sway', null, null, s.line); break;
        case 'shake': if (s.d) ex(s.d); else em('num', 0.4, null, s.line); em('shake', null, null, s.line); break;
        case 'face': ex(s.e); em('face', null, null, s.line); break;
        case 'toward': ex(s.e); em('toward', null, null, s.line); break;
        case 'move': {
          needThread(s, 'move');
          const t = s.tgt;
          if (t.k === 'xy'){ ex(t.a); ex(t.b); } else if (t.k === 'item') ex(t.a);
          if (s.speed) ex(s.speed); else em('num', 60, null, s.line);
          if (s.arc) ex(s.arc); else em('num', 0, null, s.line);
          em('move', t.k, t.trait || null, s.line); em('await', null, 'move', s.line); break;
        }
        case 'walk': needThread(s, 'walk'); ex(s.dx); if (s.speed) ex(s.speed); else em('num', 10, null, s.line); em('walk', null, null, s.line); em('await', null, 'walk', s.line); break;
        case 'hop': needThread(s, 'hop'); if (s.dx) ex(s.dx); else em('num', 0, null, s.line); if (s.h) ex(s.h); else em('num', 5, null, s.line); em('hop', null, null, s.line); em('await', null, 'hop', s.line); break;
        case 'signal': if (s.to) ex(s.to); em('signal', s.name, s.to ? 1 : 0, s.line); break;
        case 'return': em('ret', null, null, s.line); break;
        default: throw new CErr('cannot compile this statement', s.line);
      }
    }
    stmts.forEach(st);
    em(thread ? 'halt' : 'ret', null, null, stmts.length ? stmts[stmts.length - 1].line : 0);
    return code;
  }

  prog.stateIdx = C.stateIdx; prog.varIdx = C.varIdx;
  prog.bodies = prog.states.map(s => gen(s.body, true));
  const allH = [];
  prog.handlers.forEach(h => { h.state = -1; h.code = gen(h.body, false); allH.push(h); });
  prog.states.forEach((s, i) => s.handlers.forEach(h => { h.state = i; h.code = gen(h.body, false); allH.push(h); }));
  prog.allHandlers = allH;
  let bytes = 0, ops = 0;
  const all = prog.bodies.concat(allH.map(h => h.code));
  all.forEach(c => c.forEach(op => { bytes += opBytes(op); ops++; }));
  bytes += allH.length * 3 + prog.states.length * 2;   // a handler table entry and a state table entry each
  prog.bytes = bytes; prog.ops = ops;
  return prog;
}
function opBytes(op){
  switch (op.o){
    case 'num': return Number.isInteger(op.a) && op.a >= -64 && op.a <= 63 ? 1 + 0 + 1 : 3; // small ints pack into one operand byte
    case 'jmp': case 'jz': case 'jzy': case 'rpt': return 2;
    case 'bin': case 'neg': case 'not': case 'ret': case 'halt': case 'await': case 'wait': case 'stop': case 'say': case 'sway': case 'shake': case 'face': case 'toward': case 'walk': case 'hop': case 'framee': return 1;
    case 'call': case 'move': return 2;
    default: return 2;
  }
}

/* under node (js3check.js): the same functions as a module */
if (typeof module !== 'undefined' && module.exports) {
  module.exports = {CErr, lex, parse, compile, opBytes, MAXVARS, EVENTS, PARTICLES, PROPS, SENSES, CONSTS, FUNCS, FIELDS, KEYWORDS};
}
