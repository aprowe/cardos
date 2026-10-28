/* Photos on the dashboard: a page of its own (apps/photo/server, DASH_VIEWS).
 * Kept on the server with a screen copy and a print copy; the device's
 * Photos app syncs them. Runs in the dashboard's scope (server/plugins.py). */
(()=>{
"use strict";
const PH={list:[],busy:0};
async function photosLoad(){
  try{ const r=await fetch('/dash/photos',{credentials:'same-origin'}); if(r.status===403){ signedOut(); return; } PH.list=await r.json(); }
  catch(e){ toast('Couldn’t load the photos.','err'); }
  renderPhotos();
}
function renderPhotos(){
  const g=$('#ph-grid'); if(!g) return;
  $('#ph-state').textContent=PH.busy?`adding ${PH.busy}…`:PH.list.length?`${PH.list.length} photo${PH.list.length===1?'':'s'}`:'';
  g.innerHTML=PH.list.length?PH.list.map(p=>`<div class="ph"><img loading="lazy" alt="" src="/dash/photos/thumb?id=${esc(p.id)}"><div class="cap"><span title="${esc(p.name)}">${esc(p.name)}</span><button class="icon-btn" data-act="ph-del" data-id="${esc(p.id)}" title="Delete" aria-label="Delete ${esc(p.name)}">${ic('trash')}</button></div></div>`).join('')
    :'<p class="dim" style="grid-column:1/-1;margin:0">No photos yet.</p>';
}
async function photosAdd(files){
  const pics=[...files].filter(f=>/^image\//.test(f.type)||/\.(jpe?g|png|gif|webp|bmp)$/i.test(f.name));
  if(!pics.length){ toast('Those aren’t pictures.','err'); return; }
  for(const f of pics){
    PH.busy++; renderPhotos();
    try{
      const r=await fetch('/dash/photos/upload?name='+encodeURIComponent(f.name),{method:'POST',credentials:'same-origin',body:f});
      const j=await r.json().catch(()=>({}));
      if(!r.ok) throw new Error(j.error||('the server said '+r.status));
    }catch(e){ toast(`${f.name}: ${e.message}`,'err'); }
    PH.busy--;
  }
  await photosLoad();
  toast(pics.length===1?'Added. The device gets it when Photos opens.':`Added ${pics.length}. The device gets them when Photos opens.`);
}
async function photosDelete(id){
  const p=PH.list.find(x=>x.id===id);
  const r=await modal({title:'Delete this photo?',html:`<p>${esc(p?p.name:'')} goes from here, and from the device the next time it syncs.</p>`,actions:[{label:'Cancel'},{label:'Delete',value:1,kind:'danger'}]});
  if(!r) return;
  const res=await fetch('/dash/photos/photo?id='+encodeURIComponent(id),{method:'DELETE',credentials:'same-origin'});
  if(!res.ok){ toast('Couldn’t delete it.','err'); return; }
  await photosLoad();
}
(function(){
  const d=$('#ph-drop');
  ['dragenter','dragover'].forEach(ev=>d.addEventListener(ev,e=>{ e.preventDefault(); d.classList.add('over'); }));
  ['dragleave','drop'].forEach(ev=>d.addEventListener(ev,e=>{ e.preventDefault(); d.classList.remove('over'); }));
  d.addEventListener('drop',e=>{ if(e.dataTransfer&&e.dataTransfer.files.length) photosAdd(e.dataTransfer.files); });
  /* Dropped anywhere on the Photos page, not only on the box. */
  const v=$('#v-photos');
  v.addEventListener('dragover',e=>e.preventDefault());
  v.addEventListener('drop',e=>{ e.preventDefault(); d.classList.remove('over'); if(e.target!==d&&e.dataTransfer&&e.dataTransfer.files.length) photosAdd(e.dataTransfer.files); });
  $('#ph-file').onchange=e=>{ photosAdd(e.target.files); e.target.value=''; };
})();

dashPlugin('photos',{
  show: photosLoad,
  acts:{ 'ph-add':()=>$('#ph-file').click(), 'ph-del':b=>photosDelete(b.dataset.id) },
});
})();
