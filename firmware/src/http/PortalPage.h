// ---------------------------------------------------------------------------
//  PortalPage.h - the on-device single-page UI, embedded in flash.
//
//  Deliberately NOT served from LittleFS: a filesystem image is a separate
//  upload step that people forget, and a corrupt filesystem would leave a
//  bricked-looking device with no way to reconfigure it. In flash it is always
//  there, always matches the firmware, and survives everything short of a
//  reflash.
//
//  No external CSS, fonts or scripts - in AP mode there is no internet, so any
//  CDN reference would render a broken page at exactly the moment the user
//  needs it most.
// ---------------------------------------------------------------------------
#pragma once

#include <pgmspace.h>

namespace sh {

static const char kPortalHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Smart Home Node</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0e1116;--card:#171b22;--line:#252b35;--fg:#e6e9ef;--dim:#8b94a3;
--on:#22c55e;--off:#3a4150;--acc:#3b82f6;--warn:#f59e0b;--err:#ef4444}
@media(prefers-color-scheme:light){:root{--bg:#f4f6f9;--card:#fff;--line:#e2e6ec;
--fg:#111827;--dim:#6b7280;--off:#cbd5e1}}
body{background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
padding:16px;max-width:1100px;margin:0 auto;-webkit-tap-highlight-color:transparent}
h1{font-size:20px;font-weight:650}h2{font-size:14px;font-weight:600;color:var(--dim);
text-transform:uppercase;letter-spacing:.06em;margin:24px 0 10px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:4px}
.sub{color:var(--dim);font-size:13px}
.pill{font-size:12px;padding:3px 10px;border-radius:99px;border:1px solid var(--line);color:var(--dim)}
.pill.ok{color:var(--on);border-color:var(--on)}
.pill.bad{color:var(--err);border-color:var(--err)}
.pill.warn{color:var(--warn);border-color:var(--warn)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:12px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;
display:flex;flex-direction:column;gap:10px;cursor:pointer;user-select:none;transition:border-color .15s}
.tile:active{transform:scale(.985)}
.tile.on{border-color:var(--on)}
.tile .top{display:flex;align-items:center;justify-content:space-between;gap:8px}
.tile .nm{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tile .meta{font-size:12px;color:var(--dim)}
.sw{width:48px;height:28px;border-radius:99px;background:var(--off);position:relative;flex:0 0 auto;transition:background .18s}
.sw i{position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:left .18s}
.tile.on .sw{background:var(--on)}.tile.on .sw i{left:23px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:12px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
button{background:var(--acc);color:#fff;border:0;border-radius:10px;padding:10px 16px;
font:inherit;font-weight:600;cursor:pointer}
button.sec{background:transparent;border:1px solid var(--line);color:var(--fg)}
button.danger{background:var(--err)}
button:disabled{opacity:.5;cursor:default}
/* width:100% matters. Without it the fields that are not inside a .row flex
   container (the Wi-Fi password, for one) collapse to the browser's default
   input width and stop looking like the field above them. */
input,select{background:var(--bg);color:var(--fg);border:1px solid var(--line);
border-radius:10px;padding:10px 12px;font:inherit;min-width:0;flex:1;width:100%}
label{font-size:13px;color:var(--dim);display:block;margin-bottom:4px}
.f{margin-bottom:12px}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:5px 0;border-bottom:1px solid var(--line)}
td:first-child{color:var(--dim);width:45%}
.net{display:flex;justify-content:space-between;align-items:center;padding:10px 0;
border-bottom:1px solid var(--line);cursor:pointer}
.net:last-child{border:0}
.code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:22px;letter-spacing:.14em;font-weight:700}
pre{background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:10px;
font-size:11px;overflow:auto;max-height:260px;white-space:pre-wrap;word-break:break-all}
.tabs{display:flex;gap:4px;margin:16px 0 12px;border-bottom:1px solid var(--line);overflow-x:auto}
.tab{padding:9px 14px;cursor:pointer;color:var(--dim);border-bottom:2px solid transparent;white-space:nowrap}
.tab.act{color:var(--fg);border-bottom-color:var(--acc);font-weight:600}
.hide{display:none}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);background:var(--card);
border:1px solid var(--line);border-radius:10px;padding:10px 16px;opacity:0;transition:opacity .2s;pointer-events:none}
#toast.show{opacity:1}
.warn-box{border-color:var(--warn);color:var(--warn);font-size:13px}
</style></head><body>

<header>
  <div><h1 id="dname">Smart Home Node</h1><div class="sub" id="dsub">connecting…</div></div>
  <div class="row"><span class="pill" id="pLink">link</span><span class="pill" id="pTime">clock</span></div>
</header>

<div id="neterr" class="err hide"></div>

<div class="tabs">
  <div class="tab act" data-t="ctl">Control</div>
  <div class="tab" data-t="wifi">Wi-Fi</div>
  <div class="tab" data-t="set">Settings</div>
  <div class="tab" data-t="diag">Diagnostics</div>
</div>

<section id="t-ctl">
  <div class="row" style="margin-bottom:12px">
    <button class="sec" onclick="all('on')">All on</button>
    <button class="sec" onclick="all('off')">All off</button>
  </div>
  <div class="grid" id="tiles"></div>
</section>

<section id="t-wifi" class="hide">
  <div class="card">
    <h2 style="margin-top:0">Join a network</h2>
    <div class="f"><label>Network</label><div class="row">
      <input id="ssid" placeholder="SSID" autocapitalize="off" autocomplete="off">
      <button class="sec" onclick="scan()">Scan</button></div></div>
    <div class="f"><label>Password</label><input id="pass" type="password" autocomplete="off"></div>
    <button onclick="joinWifi()">Save &amp; connect</button>
    <div id="nets" style="margin-top:12px"></div>
  </div>
  <div class="card"><h2 style="margin-top:0">Saved networks</h2><div id="saved"></div></div>
</section>

<section id="t-set" class="hide">
  <div class="card">
    <h2 style="margin-top:0">Claim this device</h2>
    <p class="sub">Enter this code in the app to link the device to your account.
       Until then it will not accept cloud commands.</p>
    <div class="code" id="claim" style="margin:10px 0">--------</div>
    <div class="pill" id="claimState">unclaimed</div>
  </div>
  <div class="card">
    <h2 style="margin-top:0">Channel names</h2>
    <div id="names"></div>
    <button onclick="saveNames()" style="margin-top:10px">Save names</button>
  </div>
  <div class="card">
    <h2 style="margin-top:0">Device</h2>
    <div class="f"><label>Device name</label><input id="devname"></div>
    <div class="f"><label>Timezone (POSIX TZ)</label><input id="tz" placeholder="IST-5:30"></div>
    <button onclick="saveDevice()">Save</button>
  </div>
  <div class="card">
    <h2 style="margin-top:0">Danger zone</h2>
    <div class="row">
      <button class="sec" onclick="reboot()">Reboot</button>
      <button class="danger" onclick="factory()">Factory reset</button>
    </div>
  </div>
</section>

<section id="t-diag" class="hide">
  <div class="card" id="brownout" style="display:none">
    <div class="warn-box"><b>Last reset was a brownout.</b><br>
      The 5&nbsp;V rail sagged. Check that the JD-VCC jumper is removed and the relay
      coils have their own 2&nbsp;A supply. See docs/WIRING.md.</div>
  </div>
  <div class="card"><table id="diagtbl"></table></div>
  <div class="card"><h2 style="margin-top:0">Recent log</h2><pre id="logs">…</pre></div>
</section>

<div id="toast"></div>

<script>
const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
let ST={channels:[]}, INFO={}, ws=null, wsOk=false;

function toast(m){const t=$('#toast');t.textContent=m;t.classList.add('show');
  clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),2200);}

// Reachability banner. Without this, a device that has gone off Wi-Fi leaves
// the page sitting on "connecting…" indefinitely with no explanation - the
// cached HTML renders fine while every API call underneath it fails.
let netDown=false;
function showNetErr(){
  if(netDown)return; netDown=true;
  const e=$('#neterr');
  e.innerHTML='<b>Cannot reach the device.</b><br>'+
    'It may be restarting, or off Wi-Fi. Retrying…'+
    (devIp?'<br>If this persists, open <b>http://'+devIp+'</b> directly - '+
           'the <code>.local</code> name is unreliable once a lookup has failed.':'')+
    '<br>If its light is blinking fast, join <b>SmartHome-XXXX</b> and open '+
    '<b>192.168.4.1</b>.';
  e.classList.remove('hide');
}
function clearNetErr(){
  if(!netDown)return; netDown=false;
  $('#neterr').classList.add('hide');
}
// Self-healing address.
//
// This page is normally opened as http://smarthome-XXXX.local, and .local is
// fragile: Chrome caches a FAILED lookup, so if the device reboots while the
// page is open, every later request dies with ERR_NAME_NOT_RESOLVED even after
// the device is back. The page looks alive and nothing works.
//
// So: remember the device's own IP (it tells us in /api/info) and fall back to
// it whenever a request fails by name. An IP needs no resolver and cannot go
// stale this way.
let devIp=localStorage.getItem('sh_ip')||'';
function altBase(){
  if(!devIp)return null;
  if(location.hostname===devIp)return null;   // already on the IP
  return 'http://'+devIp;
}
async function raw(path,opt){
  const init=Object.assign({credentials:'same-origin',
    headers:{'Content-Type':'application/json'}},opt||{});
  try{
    const r=await fetch(path,init);
    clearNetErr();
    return r;
  }catch(e){
    const alt=altBase();
    if(alt){
      try{
        const r=await fetch(alt+path,init);
        clearNetErr();
        return r;
      }catch(e2){ /* the IP failed too - genuinely unreachable */ }
    }
    showNetErr(); throw e;
  }
}
async function api(path,opt,retried){
  let r=await raw(path,opt);
  // A claimed device requires local auth for writes. The claim code is printed
  // on the enclosure; exchanging it sets a session cookie for 12 hours.
  if(r.status===401&&!retried){
    const c=prompt('This device is claimed.\nEnter its claim code to unlock local control:');
    if(c){
      const a=await raw('/api/local-auth',{method:'POST',body:JSON.stringify({code:c.trim().toUpperCase()})});
      if(a.ok)return api(path,opt,true);
      toast('Wrong claim code');
    }
  }
  if(!r.ok){let m=r.status+'';try{m=(await r.json()).error||m}catch(e){}throw new Error(m);}
  return r.status===204?null:r.json();
}

$$('.tab').forEach(t=>t.onclick=()=>{
  $$('.tab').forEach(x=>x.classList.remove('act'));t.classList.add('act');
  ['ctl','wifi','set','diag'].forEach(k=>$('#t-'+k).classList.toggle('hide',k!==t.dataset.t));
  if(t.dataset.t==='diag')loadDiag();
  if(t.dataset.t==='wifi')loadSaved();
});

function renderTiles(){
  const g=$('#tiles');
  if(g.children.length!==ST.channels.length){
    g.innerHTML=ST.channels.map(c=>`<div class="tile" data-c="${c.channel}">
      <div class="top"><span class="nm"></span><span class="sw"><i></i></span></div>
      <div class="meta"></div></div>`).join('');
    $$('.tile').forEach(el=>el.onclick=()=>toggle(+el.dataset.c));
  }
  ST.channels.forEach((c,i)=>{
    const el=g.children[i];
    el.classList.toggle('on',!!c.state);
    el.querySelector('.nm').textContent=c.name;
    let m=(c.state?'ON':'OFF')+' · '+c.source;
    if(c.autoOffInSec>0)m+=' · off in '+c.autoOffInSec+'s';
    if(!c.enabled)m='disabled';
    el.querySelector('.meta').textContent=m;
  });
}

async function loadState(){ST=await api('/api/state');renderTiles();}

async function toggle(ch){
  const t=$$('.tile')[ch]; t.classList.toggle('on');   // optimistic
  try{await api('/api/relay/'+ch,{method:'POST',body:JSON.stringify({action:'toggle'})});}
  catch(e){toast('Failed: '+e.message);loadState();}
}
async function all(a){
  try{await api('/api/relay/all',{method:'POST',body:JSON.stringify({action:a})});}
  catch(e){toast('Failed: '+e.message);}
}

async function loadInfo(){
  INFO=await api('/api/info');
  // Cache the device's own address so a later name-resolution failure has
  // something concrete to fall back to.
  if(INFO.ip&&INFO.ip!=='0.0.0.0'){devIp=INFO.ip;localStorage.setItem('sh_ip',devIp);}
  $('#dname').textContent=INFO.name||'Smart Home Node';
  $('#dsub').textContent=INFO.uuid+' · fw '+INFO.firmware+' · '+INFO.relayCount+' channels';
  $('#claim').textContent=INFO.claimCode||'--------';
  const cs=$('#claimState');
  cs.textContent=INFO.claimed?'claimed':'unclaimed';
  cs.className='pill '+(INFO.claimed?'ok':'warn');
  const l=$('#pLink');
  if(INFO.wifiConnected){l.textContent=INFO.ssid+' '+INFO.rssi+'dBm';l.className='pill ok';}
  else if(INFO.apActive){l.textContent='setup mode';l.className='pill warn';}
  else{l.textContent='offline';l.className='pill bad';}
  const tm=$('#pTime');
  tm.textContent=INFO.timeSynced?INFO.time:'clock not set';
  tm.className='pill '+(INFO.timeSynced?'ok':'warn');
  $('#devname').value=INFO.name||'';
  $('#tz').value=INFO.timezone||'';
}

function renderNames(){
  $('#names').innerHTML=ST.channels.map(c=>
    `<div class="f"><label>Channel ${c.channel+1}</label>
     <input class="cn" data-c="${c.channel}" value="${(c.name||'').replace(/"/g,'&quot;')}" maxlength="23"></div>`).join('');
}
async function saveNames(){
  const channels=$$('.cn').map(i=>({index:+i.dataset.c,name:i.value.trim()}));
  try{await api('/api/settings',{method:'POST',body:JSON.stringify({channels})});
    toast('Names saved');await loadState();}
  catch(e){toast('Failed: '+e.message);}
}
async function saveDevice(){
  try{await api('/api/settings',{method:'POST',
    body:JSON.stringify({device:{name:$('#devname').value.trim(),timezone:$('#tz').value.trim()}})});
    toast('Saved');loadInfo();}
  catch(e){toast('Failed: '+e.message);}
}

// Picking a network fills the SSID and jumps straight to the password box.
// Without the jump people tap a network, see nothing happen, and miss that the
// password field above the list is the next step.
function pick(ssid){
  $('#ssid').value=ssid;
  const p=$('#pass');
  p.value='';
  p.focus();
  p.scrollIntoView({block:'center',behavior:'smooth'});
}
function renderNets(list,scanning){
  const head=scanning?'<div class="sub">Scanning…</div>':'';
  $('#nets').innerHTML=head+(list.length?list
    .sort((a,b)=>b.rssi-a.rssi).map(x=>
      `<div class="net" onclick='pick(${JSON.stringify(x.ssid)})'>
       <span>${x.ssid} ${x.known?'★':''}</span>
       <span class="sub">${x.secure?'🔒 ':''}${x.rssi} dBm</span></div>`).join('')
    :(scanning?'':'<div class="sub">No networks found</div>'));
}
// The device scans on its Wi-Fi task, not in the HTTP handler, so this polls
// until the result is fresh instead of blocking on one long request.
async function scan(){
  $('#nets').innerHTML='<div class="sub">Scanning…</div>';
  let tries=0;
  const poll=async()=>{
    try{
      const r=await api('/api/wifi/scan');
      renderNets(r.networks||[],r.scanning);
      if(r.scanning&&tries++<10)setTimeout(poll,1200);
    }catch(e){$('#nets').innerHTML='<div class="sub">Scan failed: '+e.message+'</div>';}
  };
  poll();
}
async function joinWifi(){
  const ssid=$('#ssid').value.trim();
  if(!ssid)return toast('Enter a network name');
  try{await api('/api/wifi',{method:'POST',
    body:JSON.stringify({ssid,password:$('#pass').value})});
    toast('Saved — connecting…');$('#pass').value='';setTimeout(loadInfo,4000);}
  catch(e){toast('Failed: '+e.message);}
}
async function loadSaved(){
  try{
    const s=await api('/api/wifi/saved');
    $('#saved').innerHTML=s.length?s.map(x=>
      `<div class="net"><span>${x.ssid}</span>
       <button class="sec" onclick="forget(${JSON.stringify(x.ssid)})">Forget</button></div>`).join('')
      :'<div class="sub">None saved</div>';
  }catch(e){}
}
async function forget(ssid){
  try{await api('/api/wifi/forget',{method:'POST',body:JSON.stringify({ssid})});
    toast('Forgotten');loadSaved();}catch(e){toast('Failed');}
}

async function loadDiag(){
  try{
    const d=await api('/api/diag');
    $('#brownout').style.display=d.brownout?'block':'none';
    const rows=[['Uptime',fmtUp(d.uptimeMs)],['Free heap',(d.freeHeap/1024).toFixed(1)+' KB'],
      ['Min free heap',(d.minFreeHeap/1024).toFixed(1)+' KB'],['Largest block',(d.maxAlloc/1024).toFixed(1)+' KB'],
      ['Heap fragmentation',d.heapFragPct+' %'],['Reset reason',d.resetReason],
      ['Wi-Fi',d.wifi?d.ip+' ('+d.rssi+' dBm)':'offline'],['CPU',d.cpuMhz+' MHz'],
      ['Chip temp',d.chipTempC+' °C (uncalibrated)'],['Clock',d.time||'unsynced'],
      ['MQTT',d.mqtt||'disabled'],['Relay revision',d.relayRev]];
    $('#diagtbl').innerHTML=rows.map(r=>`<tr><td>${r[0]}</td><td>${r[1]}</td></tr>`).join('');
    $('#logs').textContent=d.log||'(empty)';
  }catch(e){$('#logs').textContent='Failed: '+e.message;}
}
function fmtUp(ms){const s=Math.floor(ms/1000);const d=Math.floor(s/86400);
  const h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return (d?d+'d ':'')+(h?h+'h ':'')+m+'m';}

async function reboot(){
  if(!confirm('Reboot the device?'))return;
  try{await api('/api/reboot',{method:'POST'});toast('Rebooting…');}catch(e){}
}
async function factory(){
  const c=prompt('This erases Wi-Fi, config and pairing.\nType the claim code to confirm:');
  if(!c)return;
  try{await api('/api/factory-reset',{method:'POST',body:JSON.stringify({confirm:c})});
    toast('Factory reset — rebooting');}
  catch(e){toast('Failed: '+e.message);}
}

function connectWs(){
  try{
    ws=new WebSocket('ws://'+location.host+'/ws');
    ws.onopen=()=>{wsOk=true;};
    ws.onclose=()=>{wsOk=false;setTimeout(connectWs,3000);};
    ws.onerror=()=>{try{ws.close()}catch(e){}};
    ws.onmessage=ev=>{
      const m=JSON.parse(ev.data);
      if(m.type==='state'){ST=m.data;renderTiles();}
      else if(m.type==='relay'){
        const c=ST.channels[m.channel];
        if(c){c.state=m.state;c.source=m.source;c.rev=m.rev;renderTiles();}
      }
    };
  }catch(e){setTimeout(connectWs,3000);}
}

// Keep retrying the first load. A device that is mid-reconnect answers within
// seconds, and waiting 15 s to find that out feels broken.
async function boot(){
  let ok=false;
  try{ await loadInfo(); await loadState(); renderNames(); ok=true; }
  catch(e){ ok=false; }
  if(!ok) setTimeout(boot,3000);
  return ok;
}

(async function(){
  await boot();
  connectWs();
  // Polling is a safety net only - the WebSocket is the live path.
  setInterval(()=>{if(!wsOk)loadState().catch(()=>{})},4000);
  setInterval(()=>loadInfo().catch(()=>{}),15000);
})();
</script></body></html>)HTML";

}  // namespace sh
