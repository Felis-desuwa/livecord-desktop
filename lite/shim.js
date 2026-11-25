// LiveCord 轻量版（WebView2 外壳）注入到页面里的脚本：给网站提供和完整版一样的 window.lcDesktop 接口，
// 网站的代码（public/desktop.js、desktop-vworker.js、desktop-pcm.js）不用为轻量版改。
//
// 和完整版（Electron）不一样的地方：
//  - 外壳和页面之间只有"发一段文字"这一种通道。请求写成 "编号␟方法␟参数…"，回复和事件是 JSON。
//  - 码流、声音、预览画面走一块共享内存（外壳写、页面只读）：外壳每写进一段就发一条很短的通知（位置和长度），
//    这里把那一段拷出来，再通过页面内的 MessagePort 交给 Worker / AudioWorklet —— 所以它们看到的和完整版一模一样。
//    代价是数据要经过页面主线程（完整版是直接进 Worker 的），页面卡的时候画面和声音会跟着抖。
//  - 预览不是共享纹理，而是助手读回内存的小画面（640 宽，每秒 20 帧）。
(() => {
  'use strict';
  const wv = window.chrome && window.chrome.webview;
  if (!wv || window.top !== window) return;
  const ORIGIN = '__LC_ORIGIN__'; const SEP = '\x1f';
  if (location.origin !== ORIGIN) {
    // 断网页（外壳自己的页面）只有"重试"这一个入口
    if (!/^https?:$/.test(location.protocol)) window.lcShell = { retry: () => wv.postMessage('0' + SEP + 'retry') };
    return;
  }
  if (window.lcDesktop) return;

  let seq = 0; const pend = new Map();
  const call = (m, ...a) => new Promise((res, rej) => { const id = ++seq; pend.set(id, [res, rej]); wv.postMessage([id, m, ...a].join(SEP)); });
  const subs = { win: new Set(), audio: new Set(), video: new Set() };
  const fire = (set, s) => { for (const f of set) try { f(s); } catch {} };
  const on = (set) => (fn) => { set.add(fn); return () => set.delete(fn); };

  // ---------- 共享内存 ----------
  // 开头 16 字节是头：前 8 字节是外壳到现在一共写了多少字节。后面是环形区，每一段数据都是连续的（放不下就从头开始）
  const HDR = 16; let ring = null, ringView = null, CAP = 0, ringErr = 'no-buffer';
  wv.addEventListener('sharedbufferreceived', (e) => {
    try { const b = e.getBuffer(); ring = b; ringView = new DataView(b); CAP = (e.additionalData && e.additionalData.cap) || b.byteLength - HDR; ringErr = null; } catch (err) { ringErr = String((err && err.message) || err); }
  });
  const stat = { lost: 0, video: 0, audio: 0, preview: 0, keyReq: 0, keyN: 0, keyMsSum: 0, keyMsMax: 0, batches: 0, batchMax: 0, gapMax: 0, gap20: 0, gap50: 0 };
  let lastData = 0; // 上一批数据到的时刻：量数据是不是一阵一阵来的
  const keyAt = [0, 0]; // 向助手要关键帧的时刻（主画面 / 低档），用来量它多久能到
  function take(pos, len) {
    if (!ring) return null;
    const off = HDR + (pos % CAP); if (off + len > ring.byteLength) return null;
    const u = new Uint8Array(ring, off, len).slice();
    // 拷完再核对：页面卡了很久的话这一段可能已经被后面的数据盖掉了
    // （外壳一段最多写 4 MB，正在写的那一段也算进去）
    if (Number(ringView.getBigUint64(0, true)) - pos > CAP - (4 << 20)) { stat.lost++; return null; }
    return u;
  }

  let vid = null; // { gen, port, info, res, rej, settled }
  let aud = null; // { gen, port }
  let pvOn = true;
  function onData(items) {
    const now = performance.now(); let nv = 0; for (const it of items) if (it[0] === 1 && !((it[5] >> 1) & 1)) nv++;
    if (nv) { if (lastData) { const gap = now - lastData; if (gap > stat.gapMax) stat.gapMax = gap; if (gap > 20) stat.gap20++; if (gap > 50) stat.gap50++; } lastData = now; stat.batches++; if (nv > stat.batchMax) stat.batchMax = nv; }
    for (const [t, g, pos, len, ts, fl] of items) {
      if (t === 1) {
        const v = vid; if (!v || v.gen !== g || !v.port) continue;
        const u = take(pos, len);
        if (!u) { call('videoCmd', 'key').catch(() => {}); call('videoCmd', 'keylo').catch(() => {}); continue; } // 丢了一帧：后面的帧都接不上，要一个关键帧
        stat.video++; const ly = (fl >> 1) & 1;
        if ((fl & 1) && keyAt[ly]) { const ms = performance.now() - keyAt[ly]; keyAt[ly] = 0; stat.keyN++; stat.keyMsSum += ms; if (ms > stat.keyMsMax) stat.keyMsMax = ms; }
        v.port.postMessage({ t: ts, k: fl & 1, l: ly, d: u }, [u.buffer]);
      } else if (t === 2) {
        const a = aud; if (!a || a.gen !== g || !a.port) continue;
        const u = take(pos, len); if (u) { stat.audio++; a.port.postMessage(u, [u.buffer]); }
      } else if (t === 4) {
        const v = vid; if (!v || v.gen !== g || !v.info || !pvOn || len !== v.info.pw * v.info.ph * 4) continue;
        const u = take(pos, len); if (!u) continue;
        try { const vf = new VideoFrame(u, { format: 'BGRX', codedWidth: v.info.pw, codedHeight: v.info.ph, timestamp: ts }); stat.preview++; window.postMessage({ lcFrame: vf }, ORIGIN, [vf]); } catch {}
      }
    }
  }
  function onVideoEvent(g, d) {
    const v = vid; if (!v || v.gen !== g || !d) return;
    if (d.ev === 'init' || d.ev === 'size') {
      const info = { codec: d.codec || 'h264', w: d.w, h: d.h, srcw: d.srcw, srch: d.srch, fps: d.fps, hdr: !!d.hdr, white: d.white, enc: d.enc, tl: d.tl || 0, speed: d.speed, pw: d.pw, ph: d.ph, low: d.low || 0, loh: d.loh || 0, lofps: d.lofps || 0 };
      v.info = info;
      if (!v.settled) {
        v.settled = true;
        const ch = new MessageChannel(); v.port = ch.port1;
        // Worker 那头要关键帧（有观众进来 / 丢包）时从这个端口说
        v.port.onmessage = (e) => { if (vid === v && e.data && e.data.key) { stat.keyReq++; if (!keyAt[e.data.l ? 1 : 0]) keyAt[e.data.l ? 1 : 0] = performance.now(); call('videoCmd', e.data.l ? 'keylo' : 'key').catch(() => {}); } };
        v.res(info); window.postMessage({ lcVideoPort: true }, ORIGIN, [ch.port2]);
      } else fire(subs.video, { state: 'size', ...info });
    } else if (d.ev === 'stats') fire(subs.video, { state: 'stats', ...d });
    else if (d.ev === 'error') { if (!v.settled) { v.settled = true; v.rej(new Error(d.msg)); } else fire(subs.video, { state: 'error', detail: d.msg }); }
    else if (d.ev === 'closed') fire(subs.video, { state: 'closed' });
    else if (d.ev === 'exit') {
      if (!v.settled) { v.settled = true; v.rej(new Error('采集助手退出（' + d.code + '）')); }
      if (vid === v) { vid = null; try { v.port && v.port.close(); } catch {} fire(subs.video, { state: 'exit', code: d.code }); }
    }
  }
  wv.addEventListener('message', (e) => {
    const m = e.data; if (!m) return;
    if (m.b) return onData(m.b);
    if (m.id) { const p = pend.get(m.id); if (!p) return; pend.delete(m.id); return 'e' in m ? p[1](new Error(m.e)) : p[0](m.r); }
    if (m.ev === 'win') return fire(subs.win, m.d);
    if (m.ev === 'audio') { if (aud && aud.gen === m.g) fire(subs.audio, m.d); return; }
    if (m.ev === 'video') return onVideoEvent(m.g, m.d);
  });

  // ---------- 画面 ----------
  function videoStart(o) {
    return new Promise((res, rej) => {
      if (ringErr) return rej(new Error('共享内存不可用：' + ringErr));
      o = o || {};
      if (vid) { try { vid.port && vid.port.close(); } catch {} }
      const v = vid = { gen: -1, port: null, info: null, res, rej, settled: false };
      const kv = ['source=' + (o.test ? 'test' : String(o.sourceId || ''))];
      for (const k of ['fps', 'maxw', 'maxh', 'bitrate', 'cursor', 'preview', 'lobitrate', 'lomaxh', 'lofps', 'tl']) if (o[k] != null && Number.isFinite(Number(o[k]))) kv.push(k + '=' + Math.round(Number(o[k])));
      if (o.codec === 'av1') kv.push('codec=1');
      pvOn = o.preview == null ? true : !!Number(o.preview);
      call('videoStart', ...kv).then((g) => { v.gen = g; }, (err) => { if (vid === v) vid = null; if (!v.settled) { v.settled = true; rej(err); } });
    });
  }
  function videoStop() { const v = vid; vid = null; if (v) { try { v.port && v.port.close(); } catch {} } return call('videoStop'); }
  const num = (x, lo, hi) => { const v = Math.round(Number(x)); return v >= lo && v <= hi ? v : null; };
  async function videoBitrate(b, lo) {
    const v = num(b, 100000, 200000000); if (v != null) await call('videoCmd', 'br ' + v);
    const l = num(lo, 100000, 50000000); if (l != null) await call('videoCmd', 'brlo ' + l);
    return true;
  }

  // ---------- 声音 ----------
  async function audioStart(o) {
    if (ringErr) throw new Error('共享内存不可用：' + ringErr);
    if (aud) { try { aud.port && aud.port.close(); } catch {} }
    const a = aud = { gen: -1, port: null };
    const mode = String((o && o.mode) || 'all'); const pid = Number(o && o.pid) || 0;
    if (mode !== 'all' && !(Number.isInteger(pid) && pid > 0)) { aud = null; throw new Error('没有指定程序'); }
    const g = await call('audioStart', mode, String(pid));
    if (aud !== a) return true;
    a.gen = g; const ch = new MessageChannel(); a.port = ch.port1;
    window.postMessage({ lcAudioPort: true }, ORIGIN, [ch.port2]);
    return true;
  }
  function audioStop() { const a = aud; aud = null; if (a) { try { a.port && a.port.close(); } catch {} } return call('audioStop'); }

  // ---------- 列表：屏幕 / 窗口 / 能采集声音的程序 ----------
  const icons = new Map(); // exe 路径 -> dataURL
  async function exeIcon(p) {
    if (!p) return null;
    if (!icons.has(p)) icons.set(p, call('icon', p).catch(() => null));
    return icons.get(p);
  }
  const OVERLAY = /^(Discord Overlay|NVIDIA GeForce Overlay|NVIDIA Overlay|Steam Overlay|Xbox Game Bar|Windows 输入体验|Program Manager)$/i;
  async function sources(opts) {
    opts = opts || {};
    const tw = Math.min(Math.max(Number(opts.thumbWidth == null ? 320 : opts.thumbWidth) || 0, 0), 640);
    const types = (Array.isArray(opts.types) ? opts.types : ['screen', 'window']).filter((t) => t === 'screen' || t === 'window');
    const out = [];
    if (types.includes('screen')) {
      const scr = await call('screens', String(tw));
      scr.forEach((s, i) => out.push({ id: 'screen:' + s.idx + ':0', kind: 'screen', name: scr.length > 1 ? '屏幕 ' + (i + 1) : '整个屏幕', thumb: s.thumb || null, width: s.w, height: s.h, hz: s.hz, primary: !!s.primary, hdr: !!s.hdr }));
    }
    if (types.includes('window')) {
      const r = await call('audioList'); const own = new Set(r.own);
      for (const w of r.info.windows) {
        if (own.has(w.pid) || !w.title || OVERLAY.test(w.title)) continue;
        out.push({ id: 'window:' + w.hwnd + ':0', kind: 'window', name: w.title, thumb: null, pid: w.pid, app: w.name, icon: await exeIcon(w.path) });
      }
    }
    return out;
  }
  async function audioApps() {
    const r = await call('audioList'); const own = new Set(r.own);
    const apps = r.info.apps.filter((a) => !own.has(a.pid));
    // 还没出声的程序不在系统的音频会话里：把有窗口的程序也列上（开播后它一出声就能采到）
    for (const w of r.info.windows) if (!own.has(w.pid) && !apps.some((a) => a.pid === w.pid)) apps.push({ pid: w.pid, name: w.name, image: w.image, path: w.path, title: w.title, active: false, peak: 0, noSession: true });
    for (const a of apps) { a.icon = await exeIcon(a.path); delete a.path; }
    apps.sort((x, y) => (y.active - x.active) || (y.peak - x.peak) || x.name.localeCompare(y.name));
    return apps;
  }

  window.lcDesktop = Object.freeze({
    info: () => call('info'),
    sources,
    pickSource: async () => true, // 退回浏览器采集时用的是系统内核自己的选择框，这里选的不起作用
    audioApps, audioStart, audioStop, onAudioState: on(subs.audio),
    caps: async () => ({ native: true, av1: true, tl: true, lite: true }),
    videoStart, videoStop, videoBitrate,
    videoKey: () => call('videoCmd', 'key'),
    videoPreview: (v) => { pvOn = !!v; return call('videoCmd', 'pv ' + (v ? 1 : 0)); },
    videoSupport: () => ringErr,
    onVideoState: on(subs.video),
    setLive: (v) => call('setLive', v ? '1' : '0'),
    winState: () => call('winState'), onWinState: on(subs.win),
    metrics: () => call('metrics'),
    getCfg: () => call('getCfg'),
    setCfg: (o) => call('setCfg', 'hwAccel', o && o.hwAccel === false ? '0' : '1'),
    relaunch: () => call('relaunch'),
    _liteReset: () => { for (const k in stat) stat[k] = 0; lastData = 0; },
    _lite: () => ({ ...stat, ring: ring ? ring.byteLength : 0, ringErr }),
  });
  call('hello').catch(() => {}); // 外壳收到后把共享内存发过来

  // 轻量版用系统自己的标题栏：网站画的那条顶栏只留状态和设置按钮，不留拖动区和右上角按钮的空位
  const css = '#deskBar{padding-right:10px!important;-webkit-app-region:no-drag!important}#deskBar .desk-brand{display:none!important}';
  const addCss = () => { const s = document.createElement('style'); s.textContent = css; (document.head || document.documentElement).appendChild(s); };
  if (document.head) addCss(); else document.addEventListener('DOMContentLoaded', addCss, { once: true });
})();
