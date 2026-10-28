/* Music on the dashboard: a page of its own (apps/music/server, DASH_VIEWS).
 * The server converts what is dropped here (ffmpeg); a server without it
 * says so, and then the browser decodes whatever it can (MP3, AAC, Ogg,
 * FLAC) and resamples it to a 22050 Hz mono WAV -- about 2.6 MB a minute.
 * Quiet tracks are lifted to near full scale: the speaker is one watt.
 * Runs in the dashboard's scope (server/plugins.py). */
(()=>{
"use strict";
const MU={list:[],busy:'',playing:''};
const mmss=ms=>{ const s=Math.round(ms/1000); return Math.floor(s/60)+':'+String(s%60).padStart(2,'0'); };
async function musicLoad(){
  try{ const r=await fetch('/dash/music',{credentials:'same-origin'}); if(r.status===403){ signedOut(); return; } MU.list=await r.json(); }
  catch(e){ toast('Couldn’t load the music.','err'); }
  renderMusic();
}
function renderMusic(){
  const el=$('#mu-list'); if(!el) return;
  const total=MU.list.reduce((a,m)=>a+m.bytes,0);
  $('#mu-state').textContent=MU.busy||(MU.list.length?`${MU.list.length} track${MU.list.length===1?'':'s'}, ${(total/1048576).toFixed(1)} MB`:'');
  el.innerHTML=MU.list.length?MU.list.map(m=>`<li>
      <button class="icon-btn" data-act="mu-play" data-id="${esc(m.id)}" title="${MU.playing===m.id?'Stop':'Listen'}" aria-label="${MU.playing===m.id?'Stop':'Listen to'} ${esc(m.title)}">${MU.playing===m.id?'■':'▶'}</button>
      <span class="t" title="${esc(m.title)}">${esc(m.title)}</span>
      <span class="m">${mmss(m.ms)}</span><span class="m">${(m.bytes/1048576).toFixed(1)} MB</span>
      <button class="icon-btn" data-act="mu-del" data-id="${esc(m.id)}" title="Delete" aria-label="Delete ${esc(m.title)}">${ic('trash')}</button></li>`).join('')
    :'<li><span></span><span class="dim">No tracks yet.</span></li>';
}
async function toDeviceWav(file){
  const AC=window.AudioContext||window.webkitAudioContext;
  const ctx=new AC();
  let decoded;
  try{ decoded=await ctx.decodeAudioData(await file.arrayBuffer()); }
  finally{ ctx.close(); }
  const rate=22050, len=Math.max(1,Math.ceil(decoded.duration*rate));
  const off=new OfflineAudioContext(1,len,rate);
  const src=off.createBufferSource(); src.buffer=decoded; src.connect(off.destination); src.start();
  const pcm=(await off.startRendering()).getChannelData(0);
  let peak=0; for(let i=0;i<pcm.length;i++){ const a=Math.abs(pcm[i]); if(a>peak) peak=a; }
  const gain=peak>0.01?Math.min(4,0.95/peak):1;
  const buf=new ArrayBuffer(44+pcm.length*2), v=new DataView(buf);
  const str=(o,s)=>{ for(let i=0;i<s.length;i++) v.setUint8(o+i,s.charCodeAt(i)); };
  str(0,'RIFF'); v.setUint32(4,36+pcm.length*2,true); str(8,'WAVE'); str(12,'fmt ');
  v.setUint32(16,16,true); v.setUint16(20,1,true); v.setUint16(22,1,true); v.setUint32(24,rate,true);
  v.setUint32(28,rate*2,true); v.setUint16(32,2,true); v.setUint16(34,16,true); str(36,'data'); v.setUint32(40,pcm.length*2,true);
  for(let i=0;i<pcm.length;i++){ const s=Math.max(-1,Math.min(1,pcm[i]*gain)); v.setInt16(44+i*2,s<0?s*32768:s*32767,true); }
  return new Blob([buf],{type:'audio/wav'});
}
async function musicAdd(files){
  const tracks=[...files].filter(f=>/^audio\//.test(f.type)||/\.(mp3|m4a|aac|ogg|oga|opus|flac|wav)$/i.test(f.name));
  if(!tracks.length){ toast('Those aren’t tracks.','err'); return; }
  let added=0;
  for(const f of tracks){
    try{
      // The file as it is: the server converts it. Only a server without
      // ffmpeg says so, and then the browser converts and sends a WAV.
      MU.busy=`uploading and converting ${f.name} (${(f.size/1048576).toFixed(1)} MB)…`; renderMusic();
      let r=await fetch('/dash/music/upload?title='+encodeURIComponent(f.name),{method:'POST',credentials:'same-origin',body:f});
      let j=await r.json().catch(()=>({}));
      if(!r.ok && /ffmpeg/.test(j.error||'')){
        MU.busy=`converting ${f.name}…`; renderMusic();
        const wav=await toDeviceWav(f);
        MU.busy=`uploading ${f.name} (${(wav.size/1048576).toFixed(1)} MB)…`; renderMusic();
        r=await fetch('/dash/music/upload?title='+encodeURIComponent(f.name),{method:'POST',credentials:'same-origin',body:wav});
        j=await r.json().catch(()=>({}));
      }
      if(!r.ok) throw new Error(j.error||('the server said '+r.status));
      added++;
    }catch(e){ toast(`${f.name}: ${e.message||'this browser could not read it'}`,'err'); }
  }
  MU.busy='';
  await musicLoad();
  if(added) toast(added===1?'Added. The device gets it when Music opens.':`Added ${added}. The device gets them when Music opens.`);
}
function musicPlay(id){
  const a=$('#mu-audio');
  if(MU.playing===id){ a.pause(); MU.playing=''; renderMusic(); return; }
  a.src='/dash/music/track?id='+encodeURIComponent(id); a.play().catch(()=>{}); MU.playing=id;
  a.onended=()=>{ MU.playing=''; renderMusic(); };
  renderMusic();
}
async function musicDelete(id){
  const m=MU.list.find(x=>x.id===id);
  const r=await modal({title:'Delete this track?',html:`<p>${esc(m?m.title:'')} goes from here, and from the device the next time it syncs.</p>`,actions:[{label:'Cancel'},{label:'Delete',value:1,kind:'danger'}]});
  if(!r) return;
  const res=await fetch('/dash/music/track?id='+encodeURIComponent(id),{method:'DELETE',credentials:'same-origin'});
  if(!res.ok){ toast('Couldn’t delete it.','err'); return; }
  await musicLoad();
}
(function(){
  const d=$('#mu-drop'), v=$('#v-music');
  ['dragenter','dragover'].forEach(ev=>d.addEventListener(ev,e=>{ e.preventDefault(); d.classList.add('over'); }));
  ['dragleave','drop'].forEach(ev=>d.addEventListener(ev,e=>{ e.preventDefault(); d.classList.remove('over'); }));
  d.addEventListener('drop',e=>{ if(e.dataTransfer&&e.dataTransfer.files.length) musicAdd(e.dataTransfer.files); });
  v.addEventListener('dragover',e=>e.preventDefault());
  v.addEventListener('drop',e=>{ e.preventDefault(); d.classList.remove('over'); if(e.target!==d&&e.dataTransfer&&e.dataTransfer.files.length) musicAdd(e.dataTransfer.files); });
  $('#mu-file').onchange=e=>{ musicAdd(e.target.files); e.target.value=''; };
})();

dashPlugin('music',{
  show: musicLoad,
  acts:{ 'mu-add':()=>$('#mu-file').click(), 'mu-play':b=>musicPlay(b.dataset.id), 'mu-del':b=>musicDelete(b.dataset.id) },
});
})();
