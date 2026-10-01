"""Remote Files: the device's SD card, browsed from the dashboard.

The droplet cannot reach the device -- it is behind somebody's router -- so
the device reaches out instead, the way Claude's terminal does. While the
Remote Files app is open on it (apps/rfiles.c), it asks this server for work
over and over:

    POST /files/poll?id=N     body: the answer to job N (none on the first)
                              -> the next job, or "idle" after a short wait

One request carries an answer one way and the next job the other, so a
transfer is one round trip a chunk, not two. A job is a line and maybe data:

    ID <tab> OP <tab> PATH <tab> ARG <newline> [base64]

    list  PATH            -> "ok" and a line per entry: d|f <tab> size <tab> name
    stat  PATH            -> "ok d|f SIZE"
    read  PATH  OFF,LEN   -> "ok" and the bytes, base64
    write PATH  OFF       data: base64. Into PATH.part; OFF 0 starts it afresh,
                          any other must be the part's length so far
    commit PATH           PATH.part becomes PATH (the old one removed first)
    mkdir PATH
    rm    PATH            a file, or an empty folder
    mv    PATH  NEWPATH

and an answer is "ok ..." or "error why" on its first line. Base64 because
the device's HTTP layer carries text -- the reply is copied with %s -- and
CHUNK is what keeps a job inside the app's buffer.

The browser side is behind the dashboard's sign-in (server/dash.py): the page
at /dash/files and the calls it makes. nginx already passes /dash* through
over HTTPS, and the cookie is SameSite=Lax, so another site cannot post here
with it. Nothing is kept on the server: a download streams to the browser as
the chunks arrive, an upload is held only while it is being sent on.
"""
import base64
import html
import json
import threading
import time
import urllib.parse
from collections import deque

from . import dash

CHUNK = 3072                 # bytes a job carries: 4096 of base64
POLL_WAIT = 1.5              # how long an idle poll is held open
SEEN = 6.0                   # a device not heard from in this long is gone
ASK_TIMEOUT = 20.0           # a job not answered in this long failed
UPLOAD_MAX = 16 * 1024 * 1024


class DeviceGone(Exception):
    pass


class Broker:
    """Jobs for the device, and the answers coming back. One device: the
    first to poll is the one asked, which is the only case there is."""

    def __init__(self):
        self.cond = threading.Condition()
        self.jobs = deque()
        self.answers = {}            # id -> answer text, or None while waiting
        self.next_id = 1
        self.last_seen = 0.0

    def connected(self):
        return time.time() - self.last_seen < SEEN

    def ask(self, op, path, arg="", data="", timeout=ASK_TIMEOUT):
        """Queue a job and wait for its answer: (ok, first line, the rest)."""
        with self.cond:
            if not self.connected():
                raise DeviceGone("the device is not connected: open Remote Files on it")
            jid = self.next_id
            self.next_id += 1
            self.answers[jid] = None
            self.jobs.append("%d\t%s\t%s\t%s\n%s" % (jid, op, path, arg, data))
            self.cond.notify_all()
            end = time.time() + timeout
            while self.answers[jid] is None:
                left = end - time.time()
                if left <= 0:
                    del self.answers[jid]
                    self.jobs = deque(j for j in self.jobs
                                      if not j.startswith("%d\t" % jid))
                    raise DeviceGone("the device did not answer")
                self.cond.wait(left)
            ans = self.answers.pop(jid)
        head, _, rest = ans.partition("\n")
        if head.startswith("ok"):
            return head[2:].strip(), rest
        raise IOError(head[6:].strip() if head.startswith("error") else head or "no answer")

    def poll(self, jid, answer, wait=POLL_WAIT):
        """The device's side: hand in an answer, take the next job."""
        with self.cond:
            self.last_seen = time.time()
            if jid in self.answers and self.answers[jid] is None:
                self.answers[jid] = answer
                self.cond.notify_all()
            end = time.time() + wait
            while not self.jobs:
                left = end - time.time()
                if left <= 0:
                    return "idle\n"
                self.cond.wait(left)
            self.last_seen = time.time()
            return self.jobs.popleft()


broker = Broker()


# ---- what the page asks, in terms of jobs ------------------------------------

def clean_path(p):
    """An absolute card path, or ValueError. The device checks too."""
    p = (p or "").strip()
    if not p.startswith("/") or "\t" in p or "\n" in p:
        raise ValueError("not a path: %r" % p)
    parts = [x for x in p.split("/") if x not in ("", ".")]
    if ".." in parts:
        raise ValueError("no .. in paths")
    return "/" + "/".join(parts)


def list_dir(path):
    _, rest = broker.ask("list", path)
    out = []
    for line in rest.splitlines():
        f = line.split("\t", 2)
        if len(f) == 3 and f[0] in ("d", "f"):
            out.append({"name": f[2], "dir": f[0] == "d", "size": int(f[1] or 0)})
    out.sort(key=lambda e: (not e["dir"], e["name"].lower()))
    return out


def stat(path):
    head, _ = broker.ask("stat", path)
    kind, _, size = head.partition(" ")
    return kind == "d", int(size or 0)


def read_chunks(path, size):
    off = 0
    while off < size:
        n = min(CHUNK, size - off)
        _, rest = broker.ask("read", path, "%d,%d" % (off, n))
        data = base64.b64decode(rest.strip() or b"")
        if not data:
            raise IOError("the device read nothing at %d" % off)
        yield data
        off += len(data)


def write_file(path, data):
    off = 0
    while True:
        part = data[off:off + CHUNK]
        broker.ask("write", path, str(off), base64.b64encode(part).decode())
        off += len(part)
        if off >= len(data):
            break
    broker.ask("commit", path)


# ---- routes ----------------------------------------------------------------------

def _json(h, obj, code=200):
    h._send(code, "application/json", json.dumps(obj), (("Cache-Control", "no-store"),))


def _arg(args, k):
    return (args.get(k) or [""])[0]


def _browser(fn):
    """Behind the dashboard's cookie; the device's errors as JSON."""
    def wrapped(h, path, args):
        if not dash.logged_in(h):
            if path == "/dash/files":
                h.redirect("/dash")
            else:
                _json(h, {"error": "signed out: open /dash"}, 403)
            return
        try:
            fn(h, args)
        except DeviceGone as e:
            _json(h, {"error": str(e), "gone": True}, 503)
        except (IOError, ValueError) as e:
            _json(h, {"error": str(e)}, 400)
    wrapped.__doc__ = fn.__doc__
    return wrapped


@_browser
def get_page(h, args):
    """the card, in a browser (dashboard sign-in)"""
    h.html(dash._page("CardOS files", PAGE, here="/dash/files"))


@_browser
def get_status(h, args):
    """whether Remote Files is open on the device"""
    _json(h, {"connected": broker.connected()})


@_browser
def get_ls(h, args):
    """a folder on the card"""
    p = clean_path(_arg(args, "path") or "/")
    _json(h, {"path": p, "entries": list_dir(p)})


@_browser
def get_file(h, args):
    """a file from the card, streamed as it comes"""
    p = clean_path(_arg(args, "path"))
    is_dir, size = stat(p)
    if is_dir:
        raise ValueError("%s is a folder" % p)
    name = p.rsplit("/", 1)[-1]
    h.send_response(200)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(size))
    h.send_header("Content-Disposition",
                  "attachment; filename*=UTF-8''%s" % urllib.parse.quote(name))
    h.send_header("Cache-Control", "no-store")
    h.end_headers()
    try:
        for data in read_chunks(p, size):
            h.wfile.write(data)
    except (DeviceGone, IOError):
        h.close_connection = True       # short, and the browser says so


@_browser
def post_put(h, args):
    """a file onto the card (the body is the file)"""
    p = clean_path(_arg(args, "path"))
    data = h.body(UPLOAD_MAX)
    write_file(p, data)
    _json(h, {"ok": True, "size": len(data)})


@_browser
def post_mkdir(h, args):
    """a new folder"""
    broker.ask("mkdir", clean_path(_arg(args, "path")))
    _json(h, {"ok": True})


@_browser
def post_rm(h, args):
    """delete a file or an empty folder"""
    broker.ask("rm", clean_path(_arg(args, "path")))
    _json(h, {"ok": True})


@_browser
def post_mv(h, args):
    """rename or move"""
    broker.ask("mv", clean_path(_arg(args, "from")), clean_path(_arg(args, "to")))
    _json(h, {"ok": True})


def post_poll(h, path, args):
    """the device: an answer in, the next job out (Remote Files)"""
    try:
        jid = int(_arg(args, "id") or 0)
    except ValueError:
        jid = 0
    answer = h.body(64 * 1024).decode("utf-8", "replace")
    h.text(broker.poll(jid, answer))


ROUTES = [
    ("GET", "/dash/files", get_page, "open"),
    ("GET", "/dash/files/status", get_status, "open"),
    ("GET", "/dash/files/ls", get_ls, "open"),
    ("GET", "/dash/files/get", get_file, "open"),
    ("POST", "/dash/files/put", post_put, "open"),
    ("POST", "/dash/files/mkdir", post_mkdir, "open"),
    ("POST", "/dash/files/rm", post_rm, "open"),
    ("POST", "/dash/files/mv", post_mv, "open"),
    ("POST", "/files/poll", post_poll),
]


# ---- the page -----------------------------------------------------------------------

PAGE = """
<style>
.bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:12px}
.crumbs{flex:1;min-width:0;overflow-wrap:anywhere}
.crumbs a{color:var(--acc);text-decoration:none;cursor:pointer}
table{width:100%;border-collapse:collapse}
td{padding:7px 6px;border-top:1px solid var(--line);vertical-align:middle}
td.n{word-break:break-all}td.s{color:var(--dim);text-align:right;white-space:nowrap;width:1%}
td.a{white-space:nowrap;text-align:right;width:1%}
td.a button{padding:3px 9px;font-size:13px}
.dir{color:var(--acc);cursor:pointer;font-weight:600}
#state{font-size:13px}
#drop.over{outline:2px dashed var(--acc);outline-offset:4px}
progress{width:100%;margin-top:8px}
#editor{display:none}#editor.open{display:block}
#editor .CodeMirror{height:60vh;border:1px solid var(--line);border-radius:6px;font-size:13px}
.dirty{color:var(--bad)}
</style>
<link rel=stylesheet href="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/lib/codemirror.min.css">
<link rel=stylesheet href="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/theme/material-darker.min.css">
<script src="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/lib/codemirror.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/mode/markdown/markdown.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/mode/clike/clike.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/mode/python/python.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/mode/javascript/javascript.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/mode/properties/properties.min.js"></script>
<p id=state class=dim>checking for the device...</p>
<section id=drop>
<div class=bar><div class=crumbs id=crumbs></div>
<button onclick="mkdir()">New folder</button>
<label class="btn primary">Upload<input type=file multiple hidden onchange="upload(this.files);this.value=''"></label>
</div>
<div id=msg class=dim></div>
<progress id=prog hidden></progress>
<table id=list></table>
</section>
<section id=editor>
<div class=bar><div class=crumbs><b id=edname></b> <span id=edstate class=dim></span></div>
<button class=primary onclick="save()">Save</button>
<button onclick="closeEditor()">Close</button></div>
<textarea id=edtext></textarea>
<p class=dim>Ctrl-S saves. The file is written whole: to NAME.part, then put in place.</p>
</section>
<p class=dim>Open <b>Remote Files</b> on the device to reach its card. Drop files on the
list to upload them into this folder. Transfers are slow: a few tens of KB a second.</p>
<script>
let cwd = localStorage.getItem('cwd') || '/';
const $ = id => document.getElementById(id);
const esc = s => s.replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const join = (d, n) => (d.endsWith('/') ? d : d + '/') + n;
const size = n => n < 1024 ? n + ' B' : n < 1048576 ? (n/1024).toFixed(1) + ' KB' : (n/1048576).toFixed(1) + ' MB';
function say(t, bad) { $('msg').textContent = t || ''; $('msg').className = bad ? 'bad' : 'dim'; }
async function call(url, opts) {
  const r = await fetch(url, opts);
  const j = await r.json().catch(() => ({error: 'HTTP ' + r.status}));
  if (!r.ok || j.error) throw new Error(j.error || 'HTTP ' + r.status);
  return j;
}
function crumbs() {
  const parts = cwd.split('/').filter(Boolean);
  let h = '<a data-p="/">card</a>', p = '';
  for (const x of parts) { p += '/' + x; h += ' / <a data-p="' + esc(p) + '">' + esc(x) + '</a>'; }
  $('crumbs').innerHTML = h;
}
$('crumbs').onclick = e => { if (e.target.dataset.p) go(e.target.dataset.p); };
async function go(p) {
  cwd = p; try { localStorage.setItem('cwd', p); } catch (e) {}
  crumbs(); say('loading...');
  try {
    const j = await call('/dash/files/ls?path=' + encodeURIComponent(p));
    say('');
    const rows = j.entries.map((e, i) => (e.dir ? '<tr><td class=n data-i=' + i + '>' : '<tr><td class=n>') +
      (e.dir ? '<span class=dir>' + esc(e.name) + '/</span>'
             : '<a href="/dash/files/get?path=' + encodeURIComponent(join(cwd, e.name)) + '">' + esc(e.name) + '</a>') +
      '</td><td class=s>' + (e.dir ? '' : size(e.size)) + '</td><td class=a>' +
      (!e.dir && editable(e.name) ? '<button data-ed=' + i + '>Edit</button> ' : '') +
      '<button data-mv=' + i + '>Rename</button> <button data-rm=' + i + '>Delete</button></td></tr>');
    if (cwd !== '/') rows.unshift('<tr><td class=n colspan=3 data-up=1><span class=dir>..</span></td></tr>');
    $('list').innerHTML = rows.join('') || '<tr><td class=dim>empty</td></tr>';
    $('list').onclick = ev => {
      const t = ev.target.closest('[data-i],[data-mv],[data-rm],[data-ed],[data-up]');
      if (!t) return;
      const d = t.dataset;
      if (d.up) return go(cwd.replace(/\\/[^\\/]*$/, '') || '/');
      const e = j.entries[d.i ?? d.mv ?? d.rm ?? d.ed];
      if (!e) return;
      if (d.ed !== undefined) openEditor(e);
      else if (d.i !== undefined) go(join(cwd, e.name));
      else if (d.mv !== undefined) rename(e);
      else if (d.rm !== undefined) remove(e);
    };
  } catch (e) {
    // A remembered folder that has since gone: back to the top, saying so.
    if (p !== '/' && /no such folder/.test(e.message)) { await go('/'); say(p + ' is not there any more'); return; }
    say(e.message, true); $('list').innerHTML = '';
  }
}
async function act(url, okmsg) {
  try { await call(url, {method: 'POST'}); say(okmsg); go(cwd); } catch (e) { say(e.message, true); }
}
function mkdir() {
  const n = prompt('New folder name'); if (n) act('/dash/files/mkdir?path=' + encodeURIComponent(join(cwd, n)), 'made ' + n);
}
function rename(e) {
  const n = prompt('Rename ' + e.name + ' to', e.name);
  if (n && n !== e.name) act('/dash/files/mv?from=' + encodeURIComponent(join(cwd, e.name)) +
                              '&to=' + encodeURIComponent(n.startsWith('/') ? n : join(cwd, n)), 'renamed');
}
function remove(e) {
  if (confirm('Delete ' + e.name + (e.dir ? ' (it must be empty)' : '') + '?'))
    act('/dash/files/rm?path=' + encodeURIComponent(join(cwd, e.name)), 'deleted ' + e.name);
}
function put(file) {
  return new Promise((ok, bad) => {
    const x = new XMLHttpRequest();
    x.open('POST', '/dash/files/put?path=' + encodeURIComponent(join(cwd, file.name)));
    x.upload.onprogress = ev => { if (ev.lengthComputable) { $('prog').max = ev.total; $('prog').value = ev.loaded; } };
    x.onload = () => { let j = {}; try { j = JSON.parse(x.responseText); } catch (e) {}
                       x.status === 200 && !j.error ? ok() : bad(new Error(j.error || 'HTTP ' + x.status)); };
    x.onerror = () => bad(new Error('the upload failed'));
    x.send(file);
  });
}
async function upload(files) {
  $('prog').hidden = false;
  try {
    for (const f of files) { say('sending ' + f.name + ' (' + size(f.size) + ')...'); await put(f); }
    say('uploaded ' + files.length + ' file' + (files.length === 1 ? '' : 's'));
  } catch (e) { say(e.message, true); }
  $('prog').hidden = true; go(cwd);
}
// ---- the editor: CodeMirror 5, for the text files a card holds
const EDIT_MAX = 512 * 1024;
const MODES = {md: 'markdown', markdown: 'markdown', c: 'text/x-csrc', h: 'text/x-csrc',
               py: 'python', js: 'javascript', json: {name: 'javascript', json: true},
               ini: 'properties', cfg: 'properties', conf: 'properties',
               txt: null, log: null, csv: null};
const ext = n => n.includes('.') ? n.split('.').pop().toLowerCase() : 'txt';
const editable = n => ext(n) in MODES;
let cm = null, edPath = null, edClean = 0;
function edDirty() { return cm && !cm.isClean(edClean); }
function edShow() { $('edstate').textContent = edDirty() ? 'unsaved' : 'saved';
                    $('edstate').className = edDirty() ? 'dirty' : 'dim'; }
async function openEditor(e) {
  if (e.size > EDIT_MAX) return say(e.name + ' is too big to edit here (' + size(e.size) + ')', true);
  if (edDirty() && !confirm('Discard the changes to ' + edPath + '?')) return;
  const path = join(cwd, e.name);
  say('opening ' + e.name + '...');
  try {
    const r = await fetch('/dash/files/get?path=' + encodeURIComponent(path));
    if (!r.ok) { const j = await r.json().catch(() => ({})); throw new Error(j.error || 'HTTP ' + r.status); }
    const text = await r.text();
    if (!cm) {
      const dark = matchMedia('(prefers-color-scheme: dark)').matches;
      cm = CodeMirror.fromTextArea($('edtext'), {lineNumbers: true, lineWrapping: true,
            theme: dark ? 'material-darker' : 'default', indentUnit: 2,
            extraKeys: {'Ctrl-S': save, 'Cmd-S': save}});
      cm.on('change', edShow);
    }
    edPath = path;
    $('edname').textContent = path;
    $('editor').className = 'open';
    cm.setOption('mode', MODES[ext(e.name)]);
    cm.setValue(text);
    cm.clearHistory();
    edClean = cm.changeGeneration();
    edShow();
    cm.refresh(); cm.focus();
    say('');
  } catch (err) { say(err.message, true); }
}
async function save() {
  if (!cm || !edPath) return;
  const gen = cm.changeGeneration();
  $('edstate').textContent = 'saving...';
  try {
    await call('/dash/files/put?path=' + encodeURIComponent(edPath),
               {method: 'POST', body: new Blob([cm.getValue()], {type: 'text/plain'})});
    edClean = gen;
    edShow();
    if (edPath.startsWith(cwd === '/' ? '/' : cwd + '/')) go(cwd);
  } catch (err) { $('edstate').textContent = err.message; $('edstate').className = 'bad'; }
}
function closeEditor() {
  if (edDirty() && !confirm('Close without saving?')) return;
  $('editor').className = ''; edPath = null;
}
window.addEventListener('beforeunload', ev => { if (edDirty()) { ev.preventDefault(); ev.returnValue = ''; } });

const drop = $('drop');
drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
drop.ondragleave = () => drop.classList.remove('over');
drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); upload(e.dataTransfer.files); };
let was = null;
async function watch() {
  try {
    const j = await call('/dash/files/status');
    $('state').innerHTML = j.connected ? '<span class=ok>&#9679; the device is connected</span>'
                                       : '<span class=bad>&#9679; not connected: open Remote Files on the device</span>';
    if (j.connected && was === false) go(cwd);
    was = j.connected;
  } catch (e) { $('state').textContent = e.message; }
}
watch(); setInterval(watch, 3000); go(cwd);
</script>
"""
