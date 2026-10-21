#!/usr/bin/env python3
"""pxmon - микросервис статистики Proxmox для OLED-мониторчика (ESP8266).
Отдаёт по HTTP на порту 8123 компактные строки:
  S|node|upsec|cpu%|ramMiB|ramMaxMiB|localUnix|backupFlag
  V|vmid|name|r/s|memMiB|memMaxMiB
  D|storage|usedGiB|maxGiB   (топ-4 по размеру, по убыванию)
localUnix - время ХОСТА (локальный таймзон) для ночного режима на плате,
backupFlag - 1 если прямо сейчас идёт vzdump, иначе 0.
/pretty - HTML-дашборд для телефона/браузера (глаза + те же данные).
"""
import json
import subprocess
import time as _time
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8123

def gib(b):
    return round((b) / 1073741824.0, 1)

def mib(b):
    return int(b) // 1048576

def backup_running(tasks):
    # задача vzdump без endtime = идёт прямо сейчас
    for t in tasks if isinstance(tasks, list) else []:
        try:
            if t.get("type") == "vzdump" and not t.get("endtime"):
                return 1
        except Exception:
            pass
    return 0

TEST_FORCE_BACKUP = 0  # честные данные от /cluster/tasks
TEST_FORCE_VMDOWN = 0  # честные данные статуса VM
TEST_FORCE_DAY = 0     # честное локальное время сервера

def fetch():
    res = subprocess.run(
        ["pvesh", "get", "/cluster/resources", "--output-format", "json"],
        capture_output=True, text=True, timeout=20)
    data = json.loads(res.stdout)
    node = None
    vms = []
    stor = []
    for it in data:
        t = it.get("type")
        if t == "node":
            node = it
        elif t in ("qemu", "lxc") and not it.get("template", 0):
            vms.append(it)
        elif t == "storage" and it.get("status") == "available":
            stor.append(it)
    out = []
    tm = _time.localtime()
    sd = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec  # локальные секунды с полуночи
    if TEST_FORCE_DAY:
        sd = 36000  # 10:00 утра
    try:
        t2 = subprocess.run(
            ["pvesh", "get", "/cluster/tasks", "--output-format", "json"],
            capture_output=True, text=True, timeout=20)
        bk = backup_running(json.loads(t2.stdout))
    except Exception:
        bk = 0
    if TEST_FORCE_BACKUP:
        bk = 1
    if node:
        out.append("S|%s|%d|%.1f|%d|%d|%d|%d" % (
            node.get("node", "?"), int(node.get("uptime", 0)),
            100.0 * float(node.get("cpu", 0) or 0),
            mib(node.get("mem", 0)), mib(node.get("maxmem", 0)),
            sd, bk))

    vms.sort(key=lambda v: v.get("vmid", 0))
    for v in vms:
        name = (v.get("name") or "?")[:13].replace("|", "_")
        st = "r" if v.get("status") == "running" else "s"
        if TEST_FORCE_VMDOWN and v.get("vmid") == TEST_FORCE_VMDOWN:
            st = "s"  # ТЕСТ: притворяемся выключенной
        mem = mib(v.get("mem") or 0) if st == "r" else 0
        mm = mib(v.get("maxmem") or 0)
        out.append("V|%d|%s|%s|%d|%d" % (v.get("vmid", 0), name, st, mem, mm))
    stor.sort(key=lambda s: -(s.get("maxdisk") or 0))
    for s in stor[:4]:
        nm = (s.get("storage") or "?")[:8].replace("|", "_")
        out.append("D|%s|%.1f|%.1f" % (nm, gib(s.get("disk") or 0), gib(s.get("maxdisk") or 0)))
    return "\n".join(out) + "\n"

# ----------------------------------------------------------------------------
# ВЕБ-ДАШБОРД (порт глаз с OLED + те же данные). Один файл, без внешних скриптов.
# ----------------------------------------------------------------------------
PRETTY_HTML = r"""<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>home-srv - сервер-монитор</title>
<style>
  :root{
    --bg:#f4f4f5; --card:#ffffff; --ink:#101014; --mut:#71717a;
    --ok:#15803d; --bad:#dc2626; --bar:#0f172a; --edge:#e4e4e7;
  }
  body.night{
    --bg:#0b0b0d; --card:#17171b; --ink:#f4f4f5; --mut:#a1a1aa;
    --ok:#4ade80; --bad:#f87171; --bar:#e4e4e7; --edge:#27272a;
  }
  *{box-sizing:border-box; -webkit-tap-highlight-color:transparent;}
  body{margin:0; background:var(--bg); color:var(--ink);
       font:16px/1.35 ui-monospace,Consolas,Menlo,"Courier New",monospace;
       padding:10px 10px 24px;}
  .head{display:flex; align-items:center; gap:8px; margin:2px 4px 8px;}
  .badge{font-weight:bold; font-size:20px; padding:2px 10px; border-radius:8px;
         border:2px solid var(--edge); background:var(--card);}
  .badge.ok{color:var(--ok); border-color:var(--ok);}
  .badge.bad{color:var(--bad); border-color:var(--bad); animation:blink .9s step-start infinite;}
  .badge.vm{color:var(--bad); border-color:var(--bad);}
  .badge.bk{color:var(--ink); background:var(--ink); border-color:var(--ink);}
  @keyframes blink{50%{opacity:.35;}}
  .who{color:var(--mut); font-size:13px; margin-left:auto; text-align:right;}
  canvas{display:block; width:100%; max-width:480px; margin:0 auto 10px;
         image-rendering:pixelated; border-radius:10px; border:2px solid var(--edge);
         background:var(--card); cursor:pointer;}
  .card{background:var(--card); border:2px solid var(--edge); border-radius:10px;
        padding:8px 10px; margin:8px auto; max-width:480px;}
  .card h2{margin:0 0 6px; font-size:13px; letter-spacing:.12em; color:var(--mut);
           text-transform:uppercase;}
  .rowline{display:flex; justify-content:space-between; gap:8px; font-size:14px; margin:3px 0;}
  .dim{color:var(--mut);}
  .bar{height:8px; background:var(--edge); border-radius:4px; overflow:hidden; margin:2px 0 4px;}
  .bar i{display:block; height:100%; background:var(--bar); border-radius:4px;}
  .bar.hot i{background:var(--bad);}
  .vm{display:flex; align-items:center; gap:8px; padding:4px 0; font-size:15px;}
  .vm .st{width:22px; text-align:center; font-weight:bold;}
  .vm.on .st{color:var(--ok);}
  .vm.off{color:var(--bad); font-weight:bold;}
  .vm .mem{color:var(--mut); margin-left:auto; font-size:12px;}
  .foot{color:var(--mut); font-size:12px; text-align:center; margin-top:10px;}
  .foot button{font:inherit; padding:4px 12px; border-radius:8px; border:2px solid var(--edge);
               background:var(--card); color:var(--ink);}
  /* горизонтальный режим: глаза слева, карточки справа */
  @media (orientation: landscape) and (min-width: 640px){
    body{padding:8px 12px;}
    .wrap{display:flex; gap:10px; align-items:flex-start; max-width:940px; margin:0 auto;}
    .lcol{flex:0 0 min(44%, 480px); min-width:0;}
    .lcol canvas{width:auto; height:min(40vh, 230px); max-width:100%; margin:0;}
    .rcol{flex:1 1 auto; min-width:0;}
    .card{max-width:none; margin:0 0 8px;}
    .card:last-child{margin-bottom:0;}
  }
</style>
</head>
<body>
<div class="head">
  <span class="badge ok" id="st">...</span>
  <span class="who" id="who"></span>
</div>
<div class="wrap">
  <div class="lcol">
    <canvas id="eyes" width="640" height="320"></canvas>
  </div>
  <div class="rcol">
    <div class="card" id="cSrv">
      <h2>Server</h2>
      <div class="rowline"><span>CPU</span><b id="cpu">-</b></div>
      <div class="bar" id="bCpu"><i style="width:0%"></i></div>
      <div class="rowline"><span>RAM</span><b id="ram">-</b></div>
      <div class="bar" id="bRam"><i style="width:0%"></i></div>
      <div class="rowline"><span>uptime</span><span id="up" class="dim">-</span></div>
    </div>

    <div class="card" id="cVm">
      <h2>Virtual machines</h2>
      <div id="vms"></div>
    </div>

    <div class="card" id="cDsk">
      <h2>Disks</h2>
      <div id="disks"></div>
    </div>
  </div>
</div>

<div class="foot">
  <span id="tmr">окно данных...</span> <button onclick="update()">обновить</button>
</div>

<script>
"use strict";
var $ = function(id){ return document.getElementById(id); };
/* ---------- данные ---------- */
var data = { link:false, node:"", upsec:0, cpu:0, ram:0, ramMax:0, sd:0, backup:false,
             vms:[], disks:[], lastOk:0, lastTry:0 };
var demo = null;                    // window.__demo("down|vm|backup|night|ok") - тест лица
/* вибро-алерт (как пищалка на плате): сирена при ГП, 3 писка при VM, ночью молчит */
var vibLast = 0;
function vibrTick(){
  try{
    if (!navigator.vibrate) return;
    var m = demo || mood(), n = Date.now();
    if (m === "down"){ if (n - vibLast >= 1150){ navigator.vibrate([700, 450]); vibLast = n; } }
    else if (m === "vm"){ if (n - vibLast >= 1040){ navigator.vibrate([120, 110, 120, 110, 120, 500]); vibLast = n; } }
    else { if (vibLast){ navigator.vibrate(0); vibLast = 0; } }
  } catch(e){}
}
function update(){
  data.lastTry = Date.now();
  fetch("/", {cache:"no-store"}).then(function(r){ return r.text(); }).then(function(t){
    var d = { link:false, node:"", upsec:0, cpu:0, ram:0, ramMax:0, sd:0, backup:false,
              vms:[], disks:[] };
    t.split(/\n/).forEach(function(l){
      var p = l.split("|");
      if (p[0] === "S"){
        d.link = true; d.node = p[1]; d.upsec = +p[2]|0; d.cpu = +p[3];
        d.ram = +p[4]; d.ramMax = +p[5]; d.sd = +p[6]; d.backup = p[7] === "1";
      } else if (p[0] === "V"){
        d.vms.push({ id:+p[1], name:p[2], up:p[3] === "r", mem:+p[4], max:+p[5] });
      } else if (p[0] === "D"){
        d.disks.push({ name:p[1], used:+p[2], max:+p[3] });
      }
    });
    var lt = data.lastTry;
    if (d.link){ d.lastTry = lt; d.lastOk = Date.now(); data = d; }
    else { data.link = false; }
    renderCards();
  }).catch(function(){ data.link = false; renderCards(); });
}
function sleepFace(){ // ночь: 23:00..07:00 по серверным секундам от полуночи
  var h = (data.sd / 3600) % 24;
  return (h >= 23 || h < 7);
}
function mood(){
  if (demo) return demo;
  if (!data.link) return "down";
  var vmBad = data.vms.some(function(v){ return !v.up; });
  if (vmBad) return "vm";
  if (data.backup) return "backup";
  if (sleepFace()) return "night";
  return "ok";
}
function renderCards(){
  var m = mood();
  var st = $("st");
  var vmbad = data.vms.filter(function(v){ return !v.up; });
  if (!data.link){ st.className = "badge bad"; st.textContent = "НЕТ СВЯЗИ"; }
  else if (m === "down"){ st.className = "badge bad"; st.textContent = "УПАЛ"; }
  else {
    var extra = data.backup ? " 📦" : (sleepFace() ? " 🌙" : "");
    st.className = m === "vm" || data.backup ? "badge vm" : "badge ok";
    st.textContent = data.backup ? "БЭКАП!" :
      ((data.vms.length - vmbad.length) + "/" + data.vms.length + " VM OK" + extra);
    if (m === "vm") st.textContent = "VM ✗ " + vmbad.map(function(v){ return v.id; }).join(",");
  }
  $("who").innerHTML = "node " + (data.node || "?") +
    " / обновлено " + ago(data.lastOk) + "<br>" + (data.link ? "" : "Wi-Fi или сервер?");
  if (data.link){
    $("cpu").textContent = data.cpu.toFixed(1) + " %";
    bar($("bCpu"), data.cpu, 100, data.cpu > 85);
    $("ram").textContent = gi(data.ram) + " / " + gi(data.ramMax) + " GiB";
    bar($("bRam"), data.ram / Math.max(1, data.ramMax) * 100, 100, data.ram / data.ramMax > .9);
    $("up").textContent = upfmt(data.upsec);
    $("vms").innerHTML = data.vms.map(function(v){
      return '<div class="vm ' + (v.up ? "on" : "off") + '"><span class="st">' +
        (v.up ? "✓" : "✕") + "</span><span>" + v.id + " " + v.name +
        "</span><span class=\"mem\">" + (v.up ? gi(v.mem) : "0") + " / " + gi(v.max) + " GiB</span></div>";
    }).join("");
    $("disks").innerHTML = data.disks.map(function(s){
      var pct = Math.round(s.used / Math.max(1, s.max) * 100);
      return '<div class="rowline"><span>' + s.name + '</span><span class="dim">' +
        s.used + " / " + s.max + " GiB</span></div>" +
        '<div class="bar' + (pct > 90 ? " hot" : "") + '"><i style="width:' + pct + '%"></i></div>';
    }).join("");
  }
  document.body.classList.toggle("night", m === "night");
}
function gi(m){ return (m / 1024).toFixed(1); }
function bar(el, pct, mx, hot){ el.classList.toggle("hot", !!hot);
  el.firstElementChild.style.width = Math.max(0, Math.min(100, pct / mx * 100)) + "%"; }
function ago(t){ return t ? Math.min(59, Math.round((Date.now() - t) / 1000)) + " с назад" : "-"; }
function upfmt(s){ var d = (s / 86400) | 0, h = (s / 3600) % 24 | 0, m = (s / 60) % 60 | 0, x = s % 60 | 0;
  return (d ? d + "д " : "") + h + ":" + ("0" + m).slice(-2) + ":" + ("0" + x).slice(-2); }

/* ---------- глаза: точный порт примитивов из прошивки ---------- */
var SC = 5, cv = document.getElementById("eyes"), ctx = cv.getContext("2d");
ctx.scale(SC, SC);
var EW = 30, E1 = 18, E2 = 80, OKY = 12, OKH = 38, BADY = 2, BADH = 28, PRB = 7;
var MX = 40, MY = 36, MW = 48, MH = 24, BROWY = 8;
var INK = "#101014", SAC = "#ffffff";
var eyeL = { x:0, y:0, r:PRB, tx:0, ty:0, tr:PRB };
var eyeR = { x:0, y:0, r:PRB, tx:0, ty:0, tr:PRB };
var bro = 0, low = 0, actM = -1, actU = 0, actS = 0, nAct = 4000, nLook = 0;
var blinkOn = false, bStart = 0, dbl = false, bSec = false, nBlink = 3000;
var last = 0, T0 = Date.now();
var CSS_INK, CSS_BG, CSS_CARD;
function theme(){
  var cs = getComputedStyle(document.body);
  CSS_INK = cs.getPropertyValue("--ink").trim();
  CSS_BG = cs.getPropertyValue("--bg").trim();
  CSS_CARD = cs.getPropertyValue("--card").trim();
  if (CSS_CARD) SAC = CSS_CARD; if (CSS_INK) INK = CSS_INK;
}
function fr(x, y, w, h, c){ ctx.fillStyle = c; ctx.fillRect(x, y, w, h); }
function rrpth(x, y, w, h, r){ ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath(); }
function fillRR(x, y, w, h, r, c){ rrpth(x, y, w, h, r); ctx.fillStyle = c; ctx.fill(); }
function strokeRR(x, y, w, h, r, c){ rrpth(x, y, w, h, r); ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.stroke(); }
function dn(cx, cy, r){ ctx.beginPath(); ctx.arc(cx + .5, cy + .5, r, 0, 6.2832); ctx.fillStyle = INK; ctx.fill(); }
function dsc(cx, cy, r, c){ ctx.beginPath(); ctx.arc(cx + .5, cy + .5, r, 0, 6.2832); ctx.fillStyle = c; ctx.fill(); }
function ring(cx, cy, r, c){ ctx.beginPath(); ctx.arc(cx + .5, cy + .5, r, 0, 6.2832); ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.stroke(); }
function tri(x1, y1, x2, y2, x3, y3, c){ ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.lineTo(x3, y3);
  ctx.closePath(); ctx.fillStyle = c; ctx.fill(); }
function thick(x0, y0, x1, y1, t){ for (var i = 0; i < t; i++) ln(x0 + i, y0, x1 + i, y1, INK); }
function ln(x0, y0, x1, y1, c){ ctx.strokeStyle = c; ctx.lineWidth = 1; ctx.beginPath();
  ctx.moveTo(x0 + .5, y0 + .5); ctx.lineTo(x1 + .5, y1 + .5); ctx.stroke(); }
function hw(d, r){ if (d <= 0) return 0; if (d >= r) return r;
  return Math.floor(Math.sqrt(r * r - (r - d) * (r - d))); }

function drawEye(ex, ey, eh, e, lid, lower){
  var rr = Math.min(EW / 2, eh / 2), cy0 = ey + eh / 2;
  var pr = Math.round(e.r), cx = Math.round(ex + EW / 2 + e.x), cy = Math.round(cy0 + e.y);
  cx = Math.max(ex + pr + 2, Math.min(ex + EW - pr - 2, cx));
  cy = Math.max(ey + pr + 3, Math.min(ey + eh - pr - 3, cy));
  fillRR(ex, ey, EW, eh, rr, SAC);
  dn(cx, cy, pr);
  fr(cx - pr + 2, cy - pr + 2, 2, 2, SAC);
  if (lid > 0){ var w1 = hw(lid, rr);
    fr(ex + EW / 2 - w1, ey, w1 * 2, lid, SAC);
    fr(ex + EW / 2 - w1, ey + lid - 1, w1 * 2, 1, INK); }
  if (lower > 0){ var w2 = hw(lower, rr);
    fr(ex + EW / 2 - w2, ey + eh - lower, w2 * 2, lower, SAC);
    fr(ex + EW / 2 - w2, ey + eh - lower, w2 * 2, 1, INK); }
  strokeRR(ex, ey, EW, eh, rr, INK);
  if (bro === 1) thick(ex + 2, BROWY - 3, ex + EW - 4, BROWY - 5, 3);
  else if (bro === 2){
    if (ex < 64) thick(ex + 2, ey - 2, ex + EW - 2, ey + 4, 3);
    else thick(ex + 2, ey + 4, ex + EW - 2, ey - 2, 3);
  } else thick(ex + 2, BROWY, ex + EW - 4, BROWY - 3, 3);
}
function drawMouth(v){
  if (v === 0){ fillRR(MX, MY, MW, MH, 10, INK); var m0 = MY + MH / 2;
    for (var i = 0; i < 6; i++){ var x = MX + 4 + i * 8;
      if (i % 2 === 0) tri(x, m0 - 5, x + 8, m0 - 5, x + 4, m0 + 1, SAC);
      else tri(x, m0 + 5, x + 8, m0 + 5, x + 4, m0 - 1, SAC); }
  } else if (v === 1){ strokeRR(MX, MY, MW, MH, 10, INK);
    strokeRR(MX + 1, MY + 1, MW - 2, MH - 2, 9, INK); var m1 = MY + MH / 2;
    for (var j = 0; j < 6; j++){ var xx = MX + 4 + j * 8;
      if (j % 2 === 0) tri(xx, MY + 2, xx + 8, MY + 2, xx + 4, m1, INK);
      else tri(xx, MY + MH - 2, xx + 8, MY + MH - 2, xx + 4, m1, INK); }
  } else { dn(64, MY + MH / 2, 10); dsc(64, MY + MH / 2 - 2, 4, SAC); }
}
function both(dx, dy, r){ eyeL.tx = eyeR.tx = dx; eyeL.ty = eyeR.ty = dy;
  eyeL.tr = eyeR.tr = r; }
function rnd(a, b){ return a + Math.random() * (b - a); }

function eyesTick(now){
  if (now - last < 33) return;
  last = now;
  var t = now - T0;
  var m = mood(), bad = (m === "down" || m === "vm");
  var ey = bad ? BADY : OKY, eh = bad ? BADH : OKH;

  if (!blinkOn && now >= nBlink){ blinkOn = true; bStart = now;
    dbl = Math.random() < .2; bSec = false; }
  var lid = 0;
  if (blinkOn){ var ph = (now - bStart) / 240;
    if (ph >= 1){ if (dbl && !bSec){ bSec = true; bStart = now; ph = 0; }
      else { blinkOn = false; nBlink = now + 2000 + rnd(0, 4000); } }
    if (blinkOn) lid = Math.round((eh / 2 + 2) * Math.sin(Math.PI * ph)); }

  if (bad){ bro = 2; low = 4; both(eyeL.x, eyeL.y, 6);
    if (now >= nLook){ both(rnd(-5.5, 5.5), rnd(-5, 5), 6);
      nLook = now + 300 + rnd(0, 600); }
  } else if (m === "backup"){ actM = -1; bro = 1; low = 0;
    if (now >= nLook){ both(rnd(-.7, .7), -rnd(5.4, 6.6), 4.2);
      nLook = now + 140 + rnd(0, 160); }
  } else if (m === "night"){ actM = -1; bro = 0; low = 0;
    if (!blinkOn && nBlink < now + 5000) nBlink = now + 5000 + rnd(0, 4000);
    if (now >= nLook){ both(rnd(-2.2, 2.2), rnd(3, 5), 5.5);
      nLook = now + 3000 + rnd(0, 4000); }
  } else { low = 0;
    if (actM >= 0){ if (now >= actU){ actS++;
      if (actM === 1){ if (actS === 1){ both(-5.5, 0, PRB); actU = now + 700; }
        else if (actS === 2){ both(5.5, 0, PRB); actU = now + 700; } else actM = -1; }
      else if (actM === 2){ var wp = [[5.5,0],[0,9],[-5.5,0],[0,-9]];
        if (actS < 4){ both(wp[actS][0], wp[actS][1], PRB); actU = now + 450; } else actM = -1; }
      else actM = -1;
      if (actM === -1){ bro = 0; nAct = now + 8000 + rnd(0, 12000); nLook = now + 1000; } }
    } else { if (now >= nLook){ both(rnd(-5.5, 5.5), rnd(-9, 9), PRB);
      nLook = now + 700 + rnd(0, 2600); }
      if (now >= nAct && !bad){ actM = (Math.random() * 4) | 0; actS = 0;
        if (actM === 0){ both(0, 0, 9.5); bro = 1; actU = now + 1400; }
        else if (actM === 1){ both(-5.5, 0, PRB); actU = now + 700; }
        else if (actM === 2){ both(5.5, 0, PRB); actU = now + 450; }
        else { low = 8; both(2, 3, PRB); bro = 1; actU = now + 1600; } } }
    if (actM === -1 && m === "ok") bro = 0;
  }
  [[eyeL],[eyeR]].forEach(function(o){ var e = o[0];
    e.x += (e.tx - e.x) * .25; e.y += (e.ty - e.y) * .25; e.r += (e.tr - e.r) * .2; });

  if (m === "night" && lid < eh / 2 + 3) lid = eh / 2 + 3;

  /* кадр */
  ctx.fillStyle = SAC; ctx.fillRect(0, 0, 128, 64);
  drawEye(E1, ey, eh, eyeL, lid, low);
  drawEye(E2, ey, eh, eyeR, lid, low);
  if (bad) drawMouth(((t / 170) | 0) % 3);
  else if (m === "backup"){
    dsc(E1 + 8, 4, 6, SAC); ring(E1 + 8, 4, 6, INK);
    dsc(E2 + 16, 4, 6, SAC); ring(E2 + 16, 4, 6, INK);
    dn(64, 57, 5);
  } else if (m === "night" && ((t / 4000) | 0) % 2 === 0){
    ctx.fillStyle = INK; ctx.font = "9px monospace";
    ctx.fillText("Z", 112, 9); ctx.fillText("z", 118, 16);
  }
}
window.__demo = function(m){ demo = m; if (m) renderCards(); else update(); };

/* ---------- цикл ---------- */
function boot(){
  theme();
  update();
  setInterval(update, 60000);
  setInterval(function(){ renderCards(); vibrTick();
    var wait = Math.ceil((60000 - (Date.now() - Math.max(data.lastTry, 0))) / 1000);
    $("tmr").textContent = "обновление через " + Math.max(0, wait) + " с"; }, 1000);
  var mk = function(){ theme(); };
  new MutationObserver(mk).observe(document.body, { attributes: true, attributeFilter: ["class"] });
  requestAnimationFrame(function loop(n){ eyesTick(n); requestAnimationFrame(loop); });
}
boot();
</script>
</body>
</html>
"""

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/pretty":
            body, ctype = PRETTY_HTML, "text/html; charset=utf-8"
        else:
            try:
                body = fetch()
            except Exception as e:
                body = "E|%s\n" % str(e)[:40]
            ctype = "text/plain"
        b = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def log_message(self, *a):
        pass

if __name__ == "__main__":
    HTTPServer(("0.0.0.0", PORT), H).serve_forever()
