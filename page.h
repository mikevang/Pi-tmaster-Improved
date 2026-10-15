#pragma once

// The whole web page lives in flash. It has no external dependencies (no CDN),
// so it works on a home network even if the internet is down.
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pi-Tmaster-Improved</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:16px;font-family:system-ui,sans-serif;background:#161616;color:#eee;text-align:center}
h1{font-size:1.2em;margin:4px 0 12px}
#banner div{margin:0 auto 8px;max-width:420px;padding:8px 12px;border-radius:6px;font-size:.85em}
.warn{background:#4a2e10;border:1px solid #e67e22;color:#f5c089}
.err{background:#4a1a1a;border:1px solid #e74c3c;color:#f1a9a0}
.warn button{margin-left:8px;border:0;border-radius:4px;background:#e67e22;color:#161616;padding:2px 8px}
#tiles{display:flex;flex-wrap:wrap;justify-content:center;gap:10px;margin-bottom:14px}
.tile{background:#242424;border-radius:10px;border-top:4px solid #888;padding:10px 18px;min-width:96px}
.tile .l{font-size:.8em;color:#999}
.tile .v{font-size:1.9em;font-weight:700}
#ranges{margin:6px 0}
#ranges button{background:#2c2c2c;color:#ccc;border:0;border-radius:6px;padding:6px 12px;margin:0 3px}
#ranges button.on{background:#555;color:#fff}
canvas{width:100%;max-width:820px;height:420px;background:#1e1e1e;border-radius:8px}
#legend{font-size:.8em;margin-top:6px;color:#aaa}
#legend span{margin:0 8px}
#hstat{margin-top:6px;font-size:.75em;color:#777}
#foot{margin-top:6px;font-size:.75em;color:#777}
</style></head><body>
<h1>Pi-Tmaster-Improved BBQ Thermometer</h1>
<div id="banner"></div>
<div id="tiles"></div>
<div id="ranges"></div>
<canvas id="c"></canvas>
<div id="legend"></div>
<div id="hstat"></div>
<div id="foot"></div>
<script>
var COLORS=["#e67e22","#3498db","#9b59b6","#e74c3c"];
var RANGES=[["30 min",1800],["2 h",7200],["6 h",21600],["24 h",86400]];
var winSec=7200,hist=null,live=[],lastUp=null,rebootMsg="",offline=false,dispErr=false,errMsg="",fails=0,synced=true,histNote="",cBusy=0,hBusy=0,picoStat=null;

function $(id){return document.getElementById(id);}
function getJSON(u){
  // give up after 8 s so one stuck request (e.g. from a phone that went to sleep)
  // can never block the next ones
  var ctl=window.AbortController?new AbortController():null;
  var timer=setTimeout(function(){if(ctl)ctl.abort();},8000);
  return fetch(u,{cache:"no-store",signal:ctl?ctl.signal:undefined}).then(function(r){
    if(!r.ok)throw new Error(r.status);
    return r.text();
  }).then(function(txt){
    clearTimeout(timer);
    try{return JSON.parse(txt);}
    catch(e){   // show what the Pico actually sent, to make a bad reply easy to diagnose
      throw new Error(e.message+" | reply was "+txt.length+" bytes and begins: "+JSON.stringify(txt.slice(0,110)));
    }
  }).catch(function(e){
    clearTimeout(timer);
    throw (e&&e.name==="AbortError")?new Error("timed out"):e;
  });
}

// What the Pico says about its clock and flash storage - these decide whether history
// from before a restart can come back.
function showHistStatus(){
  var a=[];
  if(picoStat){
    a.push(picoStat.synced?"clock set":"clock NOT set");
    if(picoStat.fs===false)a.push("no flash filesystem, so history can't be saved");
    else if(picoStat.saved!==undefined)a.push(picoStat.saved+" records saved in flash");
  }
  $("hstat").textContent=histNote+(a.length?" | Thermometer: "+a.join(", "):"");
}

function renderBanner(){
  var h="";
  if(offline||dispErr)h+='<div class="err">'+(offline?"Can't reach the thermometer":"Display problem")+(errMsg?" ("+errMsg+")":"")+'</div>';
  if(!synced)h+='<div class="warn">Thermometer clock not set - history is kept since power-on but won\'t survive a restart</div>';
  if(rebootMsg)h+='<div class="warn">'+rebootMsg+'<button onclick="rebootMsg=\'\';renderBanner()">Dismiss</button></div>';
  $("banner").innerHTML=h;
}

function showCurrent(d){
  synced=!!d.synced;
  picoStat={synced:synced,fs:d.fs,saved:d.saved,ring:d.ring};
  showHistStatus();
  // uptime going DOWN between polls means the Pico restarted
  if(lastUp!==null&&d.up<lastUp)rebootMsg="Thermometer restarted around "+new Date().toLocaleTimeString();
  lastUp=d.up;
  var h="",any=false;
  for(var i=0;i<d.p.length;i++){
    if(d.p[i]===null)continue;
    any=true;
    h+='<div class="tile" style="border-top-color:'+COLORS[i]+'"><div class="l">Probe '+(i+1)+'</div><div class="v">'+d.p[i].toFixed(1)+'&deg;F</div></div>';
  }
  $("tiles").innerHTML=any?h:'<div class="tile"><div class="l">No probes connected</div></div>';
  $("foot").textContent="Updated "+new Date().toLocaleTimeString();
  // keep every reading this page receives so the graph starts moving right away
  live.push({t:Math.floor(Date.now()/1000),p:d.p});
  if(live.length>20000)live.splice(0,live.length-20000);
  draw();
}

function refreshCurrent(){
  if(Date.now()<cBusy)return Promise.resolve();   // a request is already in flight
  cBusy=Date.now()+15000;
  return getJSON("/api/current").then(function(d){
    fails=0;offline=false;dispErr=false;errMsg="";
    try{showCurrent(d);}catch(e){dispErr=true;errMsg=String(e&&e.message?e.message:e);}
    renderBanner();
  }).catch(function(e){
    // one failed poll is ignored; two in a row shows the banner (with the reason)
    fails++;
    errMsg=String(e&&e.message?e.message:e);
    if(fails>=2)offline=true;
    renderBanner();
  }).then(function(){cBusy=0;});
}

// Keep the points from a saved-history reply that is about to be replaced. If the Pico
// restarts and loses its history, the page still remembers what it was told earlier.
function archiveHist(h){
  if(!h||!h.t||!h.t.length)return;
  var seen={},j,i,n=COLORS.length;
  for(j=0;j<live.length;j++)seen[live[j].t]=1;
  for(j=0;j<h.t.length;j++){
    var t=h.t[j]+(h.off||0);
    if(seen[t])continue;
    var p=[];
    for(i=0;i<n;i++){var v=h.p[i]?h.p[i][j]:null;p.push(v===undefined?null:v);}
    live.push({t:t,p:p});
  }
  live.sort(function(a,b){return a.t-b.t;});
  if(live.length>20000)live.splice(0,live.length-20000);
}

// Fetch the history the Pico has recorded. The reply is checked before it is used, so a
// bad or partial reply can never break the graph - the last good history stays on screen.
function refreshHistory(){
  if(Date.now()<hBusy)return Promise.resolve();
  hBusy=Date.now()+15000;
  var win=winSec;
  return getJSON("/api/history?window="+win).then(function(d){
    var ok=d&&typeof d.now==="number"&&Array.isArray(d.t)&&Array.isArray(d.p)&&d.p.length>0&&
      d.p.every(function(a){return Array.isArray(a)&&a.length===d.t.length;});
    if(!ok)throw new Error("unexpected reply, fields: "+Object.keys(d||{}).join(", "));
    if(win!==winSec)return;   // the range was changed meanwhile; a newer request is on its way
    // The Pico's timestamps may be real time or seconds since power-on. Either way,
    // "now" on the Pico is "now" on this device, so shift everything by the difference.
    d.off=Math.floor(Date.now()/1000)-d.now;
    archiveHist(hist);
    hist=d;
    histNote=d.t.length?(d.t.length+" saved points from the thermometer"):"the thermometer has no saved readings in this range yet";
  }).catch(function(e){
    histNote="saved history unavailable ("+(e&&e.message?e.message:e)+") - showing only what this page has seen";
  }).then(function(){
    hBusy=0;
    showHistStatus();
    try{draw();}catch(e){$("hstat").textContent="graph problem: "+(e&&e.message?e.message:e);}
  });
}

function buildRanges(){
  var h="";
  RANGES.forEach(function(r){
    h+='<button class="'+(r[1]===winSec?"on":"")+'" onclick="setWin('+r[1]+')">'+r[0]+'</button>';
  });
  $("ranges").innerHTML=h;
}
function setWin(s){winSec=s;hBusy=0;buildRanges();refreshHistory();}

// Saved history from the Pico, plus this page's own readings for any time the saved
// history doesn't cover (before it starts, after it ends, or after a Pico restart that
// lost it). Inside the saved range the Pico's data is used as-is, so nothing doubles up.
function series(){
  var n=COLORS.length,T=[],P=[],i,k;
  for(i=0;i<n;i++)P.push([]);
  var ht=(hist&&hist.t)||[],hp=(hist&&hist.p)||[],off=(hist&&hist.off)||0;
  var first=ht.length?ht[0]+off:Infinity,last=ht.length?ht[ht.length-1]+off:-Infinity;
  function addLive(lo,hi){   // this page's readings with lo < time < hi
    for(var j=0;j<live.length;j++){
      var q=live[j];
      if(!(q.t>lo&&q.t<hi))continue;
      T.push(q.t);
      for(var m=0;m<n;m++){var v=q.p[m];P[m].push(v===undefined?null:v);}
    }
  }
  if(!ht.length){addLive(-Infinity,Infinity);return {t:T,p:P};}
  addLive(-Infinity,first);
  for(k=0;k<ht.length;k++){
    T.push(ht[k]+off);
    for(i=0;i<n;i++){var a=hp[i],h=a?a[k]:null;P[i].push(h===undefined?null:h);}
  }
  addLive(last,Infinity);
  return {t:T,p:P};
}

function draw(){
  var cv=$("c"),dpr=window.devicePixelRatio||1,W=cv.clientWidth,H=cv.clientHeight;
  if(!W)return;
  cv.width=W*dpr;cv.height=H*dpr;
  var g=cv.getContext("2d");
  g.setTransform(dpr,0,0,dpr,0,0);
  g.clearRect(0,0,W,H);
  g.font="11px system-ui,sans-serif";

  var S=series(),nowS=Math.floor(Date.now()/1000),have=S.t.length>=2;
  var L=40,R=10,T=10,B=24,pw=W-L-R,ph=H-T-B;
  var t1=Math.max(nowS,have?S.t[S.t.length-1]:0);
  // Never show less than 5 minutes, so the time axis doesn't keep rescaling while
  // the graph is young - the line simply grows in from the right.
  var t0=Math.max(t1-winSec,have?Math.min(S.t[0],t1-300):t1-300);

  // y range follows the readings on the graph: 25 degrees below the lowest and
  // 25 above the highest (only counting points inside the time range being shown)
  var mn=Infinity,mx=-Infinity;
  S.p.forEach(function(a){a.forEach(function(v,k){
    if(v!==null&&S.t[k]>=t0){if(v<mn)mn=v;if(v>mx)mx=v;}
  });});
  if(mn===Infinity){mn=125;mx=225;}   // nothing to show yet: a neutral empty frame
  var lo=Math.floor(mn-25),hi=Math.ceil(mx+25);
  function X(t){return L+(t-t0)/(t1-t0)*pw;}
  function Y(v){return T+(hi-v)/(hi-lo)*ph;}

  // gridlines every 5, 10, 25, 50, 100 or 200 degrees - whichever gives about 6 lines or fewer
  var ysteps=[5,10,25,50,100,200],ystep=200;
  for(var q=0;q<ysteps.length;q++){if((hi-lo)/ysteps[q]<=6){ystep=ysteps[q];break;}}
  g.strokeStyle="#333";g.fillStyle="#888";g.textAlign="right";g.lineWidth=1;
  for(var v=Math.ceil(lo/ystep)*ystep;v<=hi;v+=ystep){
    var y=Y(v);
    g.beginPath();g.moveTo(L,y);g.lineTo(L+pw,y);g.stroke();
    g.fillText(v,L-4,y+4);
  }

  var span=t1-t0,steps=[10,30,60,120,300,600,900,1800,3600,7200,10800,21600],tick=steps[steps.length-1];
  for(var s=0;s<steps.length;s++){if(span/steps[s]<=6){tick=steps[s];break;}}
  var fmt={hour:"2-digit",minute:"2-digit"};
  if(tick<60)fmt.second="2-digit";
  g.textAlign="center";
  for(var t=Math.ceil(t0/tick)*tick;t<=t1;t+=tick){
    var x=X(t);
    g.strokeStyle="#2a2a2a";g.beginPath();g.moveTo(x,T);g.lineTo(x,T+ph);g.stroke();
    g.fillStyle="#888";
    g.fillText(new Date(t*1000).toLocaleTimeString([],fmt),x,H-8);
  }

  // break the line where a probe was unplugged or the Pico was off
  var gap=((hist&&hist.step)||15)*3+15;
  S.p.forEach(function(a,i){
    g.strokeStyle=COLORS[i];g.lineWidth=2;g.beginPath();
    var pen=false,pt=0;
    for(var k=0;k<a.length;k++){
      if(a[k]===null||S.t[k]<t0){pen=false;continue;}
      var px=X(S.t[k]),py=Y(a[k]);
      if(pen&&S.t[k]-pt>gap)pen=false;
      if(pen)g.lineTo(px,py);else g.moveTo(px,py);
      pen=true;pt=S.t[k];
    }
    g.stroke();
  });

  // before there is a line to draw, say so on top of the empty graph
  if(!have){
    g.fillStyle="#aaa";g.textAlign="center";g.font="13px system-ui,sans-serif";
    g.fillText("Graph will appear as readings arrive...",L+pw/2,T+ph/2);
  }

  var lg="";
  S.p.forEach(function(a,i){
    if(a.some(function(v){return v!==null;}))lg+='<span style="color:'+COLORS[i]+'">&#9632; Probe '+(i+1)+'</span>';
  });
  $("legend").innerHTML=lg;
}

// A phone freezes a page's timers while it's minimised or the screen is off. When the page
// comes back, immediately fetch everything the Pico recorded in the meantime.
var lastResume=Date.now();   // page load counts as a resume, so it isn't repeated
function resume(){
  if(document.hidden)return;
  var now=Date.now();
  if(now-lastResume<2000)return;   // several events fire together; act once
  lastResume=now;
  cBusy=0;hBusy=0;                 // drop anything that was in flight when the phone slept
  refreshCurrent().then(refreshHistory);
}
document.addEventListener("visibilitychange",resume);
window.addEventListener("pageshow",resume);
window.addEventListener("focus",resume);
window.addEventListener("online",resume);

buildRanges();
histNote="Loading saved history...";showHistStatus();
draw();   // show the empty graph frame straight away
refreshCurrent().then(refreshHistory);
setInterval(refreshCurrent,3000);
setInterval(refreshHistory,30000);
window.addEventListener("resize",draw);
</script></body></html>
)rawliteral";