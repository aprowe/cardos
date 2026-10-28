/* Toggl's card on Accounts & keys (apps/toggl/server, DASH_CARDS), its row
 * on the Overview, and its running clock. What it shows is the "toggl" part
 * of /dash/api/state (dash_state() in the plugin), which asks Toggl what is
 * running -- so it is fetched on load and after an action, never on a timer.
 * Runs in the dashboard's scope (server/plugins.py). */
(()=>{
"use strict";
let tStart=0;
function elapsed(){ if(!tStart) return [0,0,0]; const s=Math.max(0,Math.floor((now()-tStart)/1000)); return [Math.floor(s/3600),Math.floor(s/60)%60,s%60]; }
function elapsedShort(){ const [h,m]=elapsed(); return `${h}:${String(m).padStart(2,'0')}`; }
let tChecking=false;
function renderToggl(){
  const el=$('#toggl'), t=S.st&&S.st.toggl;
  const head=`<div class="acct-top"><div class="logo t">⏻</div><div style="min-width:0"><h3>Toggl Track</h3><div class="sub">For the device’s Toggl app</div></div>${!t?'':t.connected?'<span class="pill ok">Connected</span>':'<span class="pill">Not connected</span>'}</div>`;
  if(!t){ el.innerHTML=head+`<p class="dim" style="margin:0">${S.stErr?esc(S.stErr):'<span class="spin"></span>Loading…'}</p>`; return; }
  if(t.connected){
    const [h,m,s]=elapsed(), r=t.running;
    el.innerHTML=head+`
      ${t.error?`<div class="note warn"><b>Couldn’t ask Toggl what’s running.</b> ${esc(t.error)}</div>`
      :r?`<div class="running"><span class="dot ok live"></span><div class="what"><b>${esc(r.description||'(no description)')}</b><span>${r.project?`<i class="proj-dot" style="background:#e57cd8"></i>${esc(r.project)}`:'No project'}</span></div><div class="clk js-toggl">${h}:${String(m).padStart(2,'0')}<small>:${String(s).padStart(2,'0')}</small></div></div>`
      :'<div class="running"><span class="dot off"></span><div class="what"><b>Nothing running</b><span>Start a timer from the device or Toggl itself.</span></div></div>'}
      <dl class="facts"><dt>Account</dt><dd>${esc(t.name||'(unknown)')}</dd><dt>Connected</dt><dd>${fmtEpoch(t.since)}</dd></dl>
      <div class="tgs" id="tgs"></div>
      <div class="acts"><button class="btn ghost" data-act="t-disconnect">Disconnect</button></div>`;
    renderTargets(); if(!TG.data&&!TG.loading&&!TG.err) loadTargets();
    return;
  }
  el.innerHTML=head+`
    <p class="dim" style="margin:0">Paste your Toggl API token and the server will check it with Toggl. The device then shows and controls the running timer.</p>
    <form class="field" id="t-form" autocomplete="off">
      <label for="t-token">API token</label>
      <div class="inp-row"><input class="inp" id="t-token" type="password" placeholder="Paste the token" spellcheck="false" ${tChecking?'disabled':''}>
        <button type="button" class="btn sm ghost" data-act="t-show" aria-label="Show token">Show</button></div>
      <span class="hint">Find it at the bottom of your profile page on track.toggl.com.</span>
      <div class="ferr" id="t-err" role="alert"></div>
      <div class="acts"><button class="btn primary" type="submit" ${tChecking?'disabled':''}>${tChecking?'<span class="spin"></span>Checking with Toggl…':'Connect'}</button></div>
    </form>`;
}
/* targets: hours a project should get, weekly or in total since a date */
const TG={data:null,err:'',loading:false};
const hm=s=>`${Math.floor(s/3600)}:${String(Math.floor(s/60)%60).padStart(2,'0')}`;
async function loadTargets(){
  TG.loading=true; renderTargets();
  try{ TG.data=await api('GET','/dash/api/toggl/targets?off='+(-new Date().getTimezoneOffset()*60)); TG.err=''; }
  catch(e){ TG.err=e.message; }
  TG.loading=false; renderTargets();
}
function renderTargets(){
  const el=$('#tgs'); if(!el) return;
  const d=TG.data, keep=$('#tg-form')?['tg-p','tg-h','tg-k','tg-s'].map(i=>$('#'+i).value):null;
  let h=`<h4>Targets <span class="faint" style="font-weight:400">— hours a project should get</span></h4>`;
  if(TG.err) h+=`<div class="note warn"><b>Couldn’t add up the hours.</b> ${esc(TG.err)}</div>`;
  if(!d){ el.innerHTML=h+(TG.err?'':'<p class="dim" style="margin:0"><span class="spin"></span>Adding up…</p>'); return; }
  if(!d.targets.length) h+='<p class="dim" style="margin:0">None yet. A weekly target restarts each Monday; a total one counts from the day the contract was booked.</p>';
  for(const t of d.targets){
    const want=t.hours*3600, pct=want?Math.min(100,t.done*100/want):0;
    const when=t.kind==='week'?'this week':`since ${new Date(t.since+'T12:00').toLocaleDateString(undefined,{day:'numeric',month:'short'})}`;
    h+=`<div class="tg"><b><i class="proj-dot" style="background:${esc(t.color)}"></i>${esc(t.name)}</b>
      <span class="n">${hm(t.done)} of ${hm(want)} ${when}</span>
      <button class="btn sm ghost" data-act="tg-del" data-p="${t.project}" data-k="${t.kind}" aria-label="Remove target">✕</button>
      <div class="bar"><i style="width:${pct.toFixed(1)}%;background:${esc(t.color)}"></i></div></div>`;
  }
  const today=new Date().toISOString().slice(0,10);
  h+=`<form class="tg-form" id="tg-form" autocomplete="off">
    <select class="inp" id="tg-p" aria-label="Project">${d.projects.map(p=>`<option value="${p.id}">${esc(p.name)}</option>`).join('')}</select>
    <input class="inp h" id="tg-h" type="number" min="0" step="0.5" placeholder="hours" aria-label="Hours">
    <select class="inp" id="tg-k" aria-label="Kind" style="flex:0 0 auto"><option value="week">a week</option><option value="total">in total</option></select>
    <input class="inp" id="tg-s" type="date" value="${today}" aria-label="Since" style="flex:0 0 auto;display:none" title="Counted from this day">
    <button class="btn sm primary" type="submit">Set</button></form>
    <span class="hint faint" style="font-size:12.5px">Setting a project again changes its target; a new total date is re-booking. Toggl keeps about three months of entries.</span>`;
  el.innerHTML=h;
  if(keep){ ['tg-p','tg-h','tg-k','tg-s'].forEach((i,n)=>{ $('#'+i).value=keep[n]; }); $('#tg-s').style.display=keep[2]==='total'?'':'none'; }
  $('#tg-k').onchange=e=>{ $('#tg-s').style.display=e.target.value==='total'?'':'none'; };
}
async function setTarget(project,kind,hours,since){
  try{ await api('POST','/dash/api/toggl/target',{project,kind,hours,since}); toast(hours?'Target set.':'Target removed.'); }
  catch(e){ toast(e.message,'err'); return; }
  loadTargets();
}

async function tConnect(){
  const v=$('#t-token').value.trim();
  if(!v){ $('#t-err').textContent='Paste a token first.'; $('#t-token').focus(); return; }
  tChecking=true; renderToggl(); $('#t-token').value=v;
  try{
    const j=await api('POST','/dash/api/toggl/token',{token:v});
    tChecking=false; toast(`Toggl connected as ${j.name||'your account'}.`);
    await loadState(true);
  }catch(e){
    tChecking=false; renderToggl(); $('#t-token').value=v;
    $('#t-err').textContent=e.message; toast('Toggl refused the token.','err');
  }
}
async function tDisconnect(){
  const r=await modal({title:'Disconnect Toggl?',html:'<p>The server forgets the API token. Toggl itself is untouched — a running timer keeps running there.</p>',actions:[{label:'Cancel'},{label:'Disconnect',value:1,kind:'danger'}]});
  if(!r) return;
  try{ await api('POST','/dash/api/toggl/forget'); toast('Toggl disconnected.'); }
  catch(e){ toast(`Couldn’t disconnect Toggl. ${e.message}`,'err'); }
  loadState(true);
}

function togglLine(t){ const r=t.running; return `${esc(r.description||'(no description)')}${r.project?' — '+esc(r.project):''}`; }
function glance(){
  const t=(S.st&&S.st.toggl)||{};
  return [['Toggl',t.error?'warn':t.connected?'ok':'off',t.connected?(t.error?'Connected, but Toggl didn’t answer just now':t.running?`Running: ${togglLine(t)}, <span class="js-toggl-short mono">${elapsedShort()}</span>`:`Connected as ${esc(t.name)}, nothing running`):'Not connected','#/accounts']];
}
function render(){
  const t=S.st&&S.st.toggl;
  if(t&&t.running) tStart=t.running.start*1000;
  renderToggl();
}
function tick(){
  const t=S.st&&S.st.toggl;
  if(t&&t.connected&&t.running){ const [h,m,s]=elapsed(); $$('.js-toggl').forEach(e=>e.innerHTML=`${h}:${String(m).padStart(2,'0')}<small>:${String(s).padStart(2,'0')}</small>`); $$('.js-toggl-short').forEach(e=>e.textContent=elapsedShort()); }
}
document.addEventListener('submit',e=>{
  if(e.target.id==='t-form'){ e.preventDefault(); tConnect(); }
  if(e.target.id==='tg-form'){ e.preventDefault(); const h=parseFloat($('#tg-h').value); if(!(h>0)){ $('#tg-h').focus(); return; }
    const k=$('#tg-k').value; setTarget($('#tg-p').value,k,h,k==='total'?$('#tg-s').value:undefined); }
});
dashPlugin('toggl',{
  render, tick, glance,
  acts:{
    't-disconnect':tDisconnect,
    'tg-del':b=>setTarget(b.dataset.p,b.dataset.k,0),
    't-show':b=>{ const i=$('#t-token'); i.type=i.type==='password'?'text':'password'; b.textContent=i.type==='password'?'Show':'Hide'; },
  },
});
})();
