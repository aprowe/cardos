/* Notes on the dashboard: a page of its own (apps/notes/server, DASH_VIEWS).
 * Kept on the server; the device's Notes app syncs its own copies.
 * Runs in the dashboard's scope (server/plugins.py says what it may use). */
(()=>{
"use strict";
const N={list:[],id:null,clean:'',busy:false};
const noteDirty=()=>$('#note-text').value!==N.clean;
function noteMeta(t){ $('#note-meta').textContent=t||''; }
function renderNotes(){
  const el=$('#notes-list');
  el.innerHTML=N.list.length?N.list.map(n=>`<button data-act="note-open" data-id="${esc(n.id)}" class="${n.id===N.id?'on':''}"><span class="t">${esc(n.title)}</span><span class="w">${esc(new Date(n.updated*1000).toLocaleString(undefined,{day:'numeric',month:'short',hour:'2-digit',minute:'2-digit'}))}</span></button>`).join(''):'<p class="faint" style="padding:8px">No notes yet.</p>';
  $('#note-state').textContent=N.busy?'saving…':(N.id&&noteDirty()?'unsaved changes':'');
}
async function noteFetch(method,url,body){
  const r=await fetch(url,{method,credentials:'same-origin',body});
  if(r.status===403){ signedOut(); throw new ApiError('You’re signed out.'); }
  const text=await r.text();
  if(!r.ok) throw new ApiError(text.replace(/^error /,'').trim()||('HTTP '+r.status));
  return text;
}
async function notesLoad(){
  try{ N.list=JSON.parse(await noteFetch('GET','/dash/notes')); renderNotes(); }
  catch(e){ toast(e.message,'err'); }
}
function noteGuard(){ return !noteDirty()||confirm('Discard the unsaved changes to this note?'); }
async function noteOpen(id){
  if(id===N.id||!noteGuard()) return;
  try{
    const text=await noteFetch('GET','/dash/notes/note?id='+encodeURIComponent(id));
    N.id=id; N.clean=text; $('#note-text').value=text;
    const n=N.list.find(n=>n.id===id); noteMeta(n?'saved '+new Date(n.updated*1000).toLocaleString():'');
    renderNotes();
  }catch(e){ toast(e.message,'err'); }
}
function noteNew(){
  if(!noteGuard()) return;
  N.id=null; N.clean=''; $('#note-text').value=''; noteMeta('a new note'); renderNotes(); $('#note-text').focus();
}
async function noteSave(){
  const text=$('#note-text').value;
  if(!text.trim()){ toast('Write something first — the first line is the title.','err'); return; }
  N.busy=true; renderNotes();
  try{
    const line=await noteFetch('POST','/dash/notes/note'+(N.id?'?id='+encodeURIComponent(N.id):''),text);
    N.id=line.split('\t')[0]; N.clean=text; noteMeta('saved just now');
    await notesLoad();
  }catch(e){ toast(e.message,'err'); }
  finally{ N.busy=false; renderNotes(); }
}
async function noteDelete(){
  if(!N.id){ noteNew(); return; }
  if(!confirm('Delete this note? The device deletes its copy at its next sync.')) return;
  try{ await noteFetch('DELETE','/dash/notes/note?id='+encodeURIComponent(N.id)); N.id=null; N.clean=''; $('#note-text').value=''; noteMeta(''); await notesLoad(); toast('Deleted.','info'); }
  catch(e){ toast(e.message,'err'); }
}
$('#note-text').addEventListener('input',()=>{ $('#note-state').textContent=noteDirty()?'unsaved changes':''; });
$('#note-text').addEventListener('keydown',e=>{ if((e.ctrlKey||e.metaKey)&&e.key==='s'){ e.preventDefault(); noteSave(); } });
window.addEventListener('beforeunload',e=>{ if(N.id!==undefined&&noteDirty()){ e.preventDefault(); e.returnValue=''; } });

dashPlugin('notes',{
  show: notesLoad,
  acts:{ 'note-open':b=>noteOpen(b.dataset.id), 'note-new':noteNew, 'note-save':noteSave, 'note-delete':noteDelete },
});
})();
