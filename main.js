'use strict';
// LiveCord 桌面版 —— 主进程
//
// 界面本身就是线上网站（窗口直接加载 https://live.felisnyan.com）：服务器对写请求和实时连接核对来源，
// 从本地文件加载会被全部拒绝；直接加载还有一个好处，界面随网站更新，不用重装软件。
// 这个壳只做浏览器做不到的事：
//   - 固定显卡编码接口（关掉 D3D12 编码，走 MediaFoundation，游戏占满显卡时 120 帧不排队）
//   - 自己的画面来源选择（屏幕 / 窗口缩略图），不弹浏览器的共享框
//   - 按进程采集声音（native/lc-audio.cpp），只播游戏、不带 Discord
// 不做任何主动提醒：没有系统通知、托盘气泡、任务栏闪烁、角标。
const { app, BrowserWindow, session, ipcMain, desktopCapturer, shell, Menu, dialog, powerSaveBlocker, MessageChannelMain, screen, sharedTexture } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn, execFile } = require('child_process');

const PROD_URL = 'https://live.felisnyan.com';
// 开发时指到本地实例：--lc-url=http://localhost:3187（只认本机地址，装好的软件不会被一条命令行带去别的网站）
function resolveUrl() {
  const arg = process.argv.find((a) => a.startsWith('--lc-url='));
  const u = (arg ? arg.slice(9) : process.env.LC_URL) || '';
  try { const p = new URL(u); if (['localhost', '127.0.0.1'].includes(p.hostname)) return p.origin; } catch {}
  return PROD_URL;
}
if (process.env.LC_USER_DATA) app.setPath('userData', process.env.LC_USER_DATA); // 测试时用独立的数据目录
const APP_URL = resolveUrl();
const APP_ORIGIN = new URL(APP_URL).origin;
const fromApp = (url) => { try { return new URL(url).origin === APP_ORIGIN; } catch { return false; } };

// ---------- 设置（存在用户数据目录，不走网站的 localStorage：有些要在窗口创建前就知道） ----------
const CFG_FILE = () => path.join(app.getPath('userData'), 'desktop.json');
const CFG_DEFAULT = { bounds: null, maximized: false, hwAccel: true, encodeGpu: 'auto' };
let cfg = { ...CFG_DEFAULT };
function loadCfg() { try { cfg = { ...CFG_DEFAULT, ...JSON.parse(fs.readFileSync(CFG_FILE(), 'utf8')) }; } catch {} }
let cfgTimer = null;
function saveCfg() { clearTimeout(cfgTimer); cfgTimer = setTimeout(() => { try { fs.writeFileSync(CFG_FILE(), JSON.stringify(cfg, null, 2)); } catch {} }, 300); }
loadCfg();

// ---------- 内核启动参数 ----------
// D3D12 编码接口在游戏占满显卡时会排队掉帧（实测 120 帧档被压到 45～80 帧），MediaFoundation 同负载稳定 115 帧
const DISABLE = ['D3D12VideoEncodeAccelerator', 'HardwareMediaKeyHandling', 'MediaSessionService', 'WidgetLayering'];
const ENABLE = [];
const extraDisable = (process.env.LC_DISABLE_FEATURES || '').split(',').filter(Boolean);
const extraEnable = (process.env.LC_ENABLE_FEATURES || '').split(',').filter(Boolean);
app.commandLine.appendSwitch('disable-features', [...DISABLE, ...extraDisable].join(','));
if (ENABLE.length || extraEnable.length) app.commandLine.appendSwitch('enable-features', [...ENABLE, ...extraEnable].join(','));
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required'); // 音效和直播声音不需要先点一下页面
// 原生采集发两档画面时，发送端编的是两路极小的占位画面（见 public/desktop-vworker.js）。
// 内核默认按分辨率限制 simulcast 的路数（低于 480×270 只给一路），这里放开，占位画面才能做得很小、几乎不花 CPU
app.commandLine.appendSwitch('force-fieldtrials', 'WebRTC-LegacySimulcastLayerLimit/Disabled/');
if (!cfg.hwAccel) app.disableHardwareAcceleration();
app.setAppUserModelId('com.livecord.desktop');

if (!app.requestSingleInstanceLock()) { app.quit(); return; }

const HIDDEN = process.argv.includes('--lc-hidden'); // 不显示窗口（自动化测试用：不抢正在玩的游戏的焦点）
let win = null;
let live = false; // 页面告诉我们是否正在直播（关窗口前确认、阻止系统休眠）
let psb = null;
let pendingSource = null; // 页面在自己的选择界面里选好的画面来源，下一次 getDisplayMedia 用它

function send(ch, ...a) { if (win && !win.isDestroyed()) win.webContents.send(ch, ...a); }
function winState() {
  if (!win || win.isDestroyed()) return { focused: false, minimized: false, visible: false, maximized: false, fullscreen: false };
  return { focused: win.isFocused(), minimized: win.isMinimized(), visible: win.isVisible(), maximized: win.isMaximized(), fullscreen: win.isFullScreen() };
}

function createWindow() {
  const b = cfg.bounds;
  const onScreen = b && screen.getAllDisplays().some((d) => b.x < d.bounds.x + d.bounds.width - 80 && b.x + b.width > d.bounds.x + 80 && b.y >= d.bounds.y - 10 && b.y < d.bounds.y + d.bounds.height - 60);
  win = new BrowserWindow({
    width: b?.width || 1280, height: b?.height || 800, ...(onScreen ? { x: b.x, y: b.y } : {}),
    minWidth: 940, minHeight: 560,
    show: false, backgroundColor: '#1e1f22', title: 'LiveCord',
    icon: path.join(__dirname, 'build', 'icon.ico'),
    titleBarStyle: 'hidden',
    titleBarOverlay: { color: '#1e1f22', symbolColor: '#b5bac1', height: 32 },
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true, sandbox: true, nodeIntegration: false, webSecurity: true,
      backgroundThrottling: false, // 窗口最小化 / 被游戏挡住时，采集和统计不能降速
      spellcheck: false,
    },
  });
  if (cfg.maximized) win.maximize();
  win.once('ready-to-show', () => { if (!HIDDEN) win.show(); });
  const remember = () => { if (!win.isMinimized() && !win.isMaximized() && !win.isFullScreen()) cfg.bounds = win.getBounds(); cfg.maximized = win.isMaximized(); saveCfg(); };
  win.on('resize', remember); win.on('move', remember);
  for (const ev of ['focus', 'blur', 'minimize', 'restore', 'show', 'hide', 'maximize', 'unmaximize', 'enter-full-screen', 'leave-full-screen'])
    win.on(ev, () => send('lc:win-state', winState()));
  win.on('close', (e) => {
    if (!live) return;
    const r = dialog.showMessageBoxSync(win, { type: 'question', buttons: ['结束直播并退出', '取消'], defaultId: 1, cancelId: 1, title: 'LiveCord', message: '正在直播，确定要退出吗？', detail: '退出后直播会立刻结束。', noLink: true });
    if (r !== 0) e.preventDefault();
  });
  win.on('closed', () => { win = null; stopAudio(); stopVideo(); });

  const wc = win.webContents;
  // 只在本站内跳转；其他链接交给系统浏览器
  wc.on('will-navigate', (e, url) => { if (!fromApp(url) && !url.startsWith('file:')) { e.preventDefault(); if (/^https?:/i.test(url)) shell.openExternal(url); } });
  wc.setWindowOpenHandler(({ url }) => { if (/^https?:/i.test(url)) shell.openExternal(url); return { action: 'deny' }; });
  wc.on('did-fail-load', (e, code, desc, url, isMain) => { if (isMain && code !== -3 && !url.startsWith('file:')) loadOffline(desc); });
  wc.on('render-process-gone', (e, d) => { stopAudio(); if (d.reason !== 'clean-exit' && d.reason !== 'killed') setTimeout(() => { if (win && !win.isDestroyed()) win.loadURL(APP_URL); }, 500); });
  wc.on('did-start-navigation', (e, url, inPlace, isMain) => { if (isMain && !inPlace) { live = false; setLive(false); stopAudio(); stopVideo(); } });
  wc.on('before-input-event', (e, input) => {
    if (input.type !== 'keyDown') return;
    if (input.key === 'F12' || (input.control && input.shift && input.key.toLowerCase() === 'i')) { wc.toggleDevTools(); e.preventDefault(); }
    else if (input.key === 'F5' || (input.control && input.key.toLowerCase() === 'r')) { if (!live) wc.reload(); e.preventDefault(); } // 直播中不让误触刷新
  });
  win.loadURL(APP_URL);
}
function loadOffline(desc) {
  if (!win || win.isDestroyed()) return;
  win.loadFile(path.join(__dirname, 'offline.html'), { query: { url: APP_URL, why: String(desc || '') } });
}
function setLive(on) {
  live = !!on;
  if (live && psb == null) psb = powerSaveBlocker.start('prevent-app-suspension');
  else if (!live && psb != null) { powerSaveBlocker.stop(psb); psb = null; }
}

// ---------- 画面来源 ----------
const iconCache = new Map(); // exe 路径 -> dataURL
async function exeIcon(p) {
  if (!p) return null;
  if (iconCache.has(p)) return iconCache.get(p);
  let url = null;
  try { const img = await app.getFileIcon(p, { size: 'normal' }); if (!img.isEmpty()) url = img.toDataURL(); } catch {}
  iconCache.set(p, url);
  return url;
}
const knownSources = new Map(); // id -> 最近一次列出来的来源
const OVERLAY = /^(Discord Overlay|NVIDIA GeForce Overlay|NVIDIA Overlay|Steam Overlay|Xbox Game Bar|Windows 输入体验|Program Manager)$/i;
async function listSources(opts = {}) {
  // 窗口缩略图很慢（前台是全屏游戏时一次要好几秒），所以页面分开要：屏幕带缩略图，窗口先不带、之后再补
  const tw = Math.min(Math.max(Number(opts.thumbWidth ?? 320) || 0, 0), 640);
  const types = (Array.isArray(opts.types) ? opts.types : ['screen', 'window']).filter((t) => t === 'screen' || t === 'window');
  const [sources, info] = await Promise.all([
    desktopCapturer.getSources({ types, thumbnailSize: { width: tw, height: Math.round(tw * 9 / 16) }, fetchWindowIcons: false }),
    types.includes('window') ? audioInfo().catch(() => ({ apps: [], windows: [] })) : { apps: [], windows: [] },
  ]);
  for (const s of sources) knownSources.set(s.id, { id: s.id, name: s.name, display_id: s.display_id });
  const displays = screen.getAllDisplays(); const primary = screen.getPrimaryDisplay().id;
  const selfIds = new Set(BrowserWindow.getAllWindows().map((w) => w.getMediaSourceId()));
  const out = [];
  for (const s of sources) {
    if (selfIds.has(s.id)) continue; // 不列自己的窗口
    const kind = s.id.startsWith('screen:') ? 'screen' : 'window';
    const item = { id: s.id, kind, name: s.name, thumb: s.thumbnail.isEmpty() ? null : s.thumbnail.toDataURL() };
    if (kind === 'screen') {
      const d = displays.find((x) => String(x.id) === String(s.display_id));
      if (d) { const px = screen.dipToScreenRect(null, d.bounds); item.width = px.width; item.height = px.height; item.hz = Math.round(d.displayFrequency || 0); item.primary = d.id === primary; item.hdr = !!(d.colorSpace && /hdr|pq|scrgb/i.test(d.colorSpace)) || d.colorDepth > 24; }
      item.name = displays.length > 1 ? `屏幕 ${out.filter((x) => x.kind === 'screen').length + 1}` : '整个屏幕';
    } else {
      const hwnd = s.id.split(':')[1];
      const w = info.windows.find((x) => String(x.hwnd) === hwnd);
      if (!w) continue; // 助手只列有标题、非工具窗口的顶层窗口；不在里面的是各种浮层
      if (OVERLAY.test(s.name)) continue;
      item.pid = w.pid; item.app = w.name; item.icon = await exeIcon(w.path);
    }
    out.push(item);
  }
  return out;
}

// ---------- 声音：按进程采集 ----------
const HELPER = app.isPackaged ? path.join(process.resourcesPath, 'bin', 'lc-audio.exe') : path.join(__dirname, 'bin', 'lc-audio.exe');
function audioInfo() {
  return new Promise((resolve, reject) => {
    execFile(HELPER, ['list'], { windowsHide: true, timeout: 5000, maxBuffer: 4 << 20, encoding: 'utf8' }, (err, stdout) => {
      if (err) return reject(err);
      try { resolve(JSON.parse(stdout)); } catch (e) { reject(e); }
    });
  });
}
const ownPids = () => new Set([process.pid, ...app.getAppMetrics().map((m) => m.pid)]);
async function listAudioApps() {
  const info = await audioInfo(); const mine = ownPids();
  const apps = info.apps.filter((a) => !mine.has(a.pid));
  // 还没出声的程序不在系统的音频会话里：把有窗口的程序也列上（开播后它一出声就能采到）
  for (const w of info.windows) if (!mine.has(w.pid) && !apps.some((a) => a.pid === w.pid)) apps.push({ pid: w.pid, name: w.name, image: w.image, path: w.path, title: w.title, active: false, peak: 0, noSession: true });
  for (const a of apps) { a.icon = await exeIcon(a.path); delete a.path; }
  apps.sort((x, y) => (y.active - x.active) || (y.peak - x.peak) || x.name.localeCompare(y.name));
  return apps;
}

let audio = null; // { proc, port, key }
function stopAudio() {
  const a = audio; audio = null;
  if (!a) return;
  try { a.proc.stdin.end(); } catch {}
  setTimeout(() => { try { a.proc.kill(); } catch {} }, 500);
  try { a.port.close(); } catch {}
}
// mode: 'app'（只要这个程序）| 'except'（全部声音但不含这个程序）| 'all'（全部声音，不含本软件自己）
function startAudio({ mode, pid }) {
  stopAudio();
  let args;
  if (mode === 'app') args = ['capture', String(pid), 'include'];
  else if (mode === 'except') args = ['capture', String(pid), 'exclude'];
  else args = ['capture', String(process.pid), 'exclude']; // 排除自己这棵进程树：别人直播的声音和音效不会被播回去
  if (mode !== 'all' && !(Number.isInteger(pid) && pid > 0)) throw new Error('没有指定程序');
  const proc = spawn(HELPER, args, { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  const { port1, port2 } = new MessageChannelMain();
  const me = audio = { proc, port: port1 };
  let rem = null; // 管道给的数据不保证按整帧（8 字节）切
  proc.stdout.on('data', (d) => {
    if (audio !== me) return;
    if (rem) { d = Buffer.concat([rem, d]); rem = null; }
    const n = d.length - (d.length % 8);
    if (n < d.length) rem = d.subarray(n);
    if (n) port1.postMessage(new Uint8Array(d.buffer.slice(d.byteOffset, d.byteOffset + n)));
  });
  let errBuf = '';
  proc.stderr.on('data', (d) => {
    errBuf += d;
    if (audio !== me) return;
    if (/ready/.test(errBuf)) { send('lc:audio-state', { state: 'ready' }); errBuf = errBuf.replace(/ready\s*/, ''); }
  });
  proc.on('error', (e) => { if (audio === me) { audio = null; send('lc:audio-state', { state: 'error', detail: e.message }); } });
  proc.on('exit', (code) => {
    if (audio !== me) return;
    audio = null; try { port1.close(); } catch {}
    send('lc:audio-state', { state: code === 3 ? 'target-exit' : 'error', detail: errBuf.trim() });
  });
  port1.start();
  win.webContents.postMessage('lc:audio-port', null, [port2]);
}

// ---------- 画面：原生采集 + 编码（native/lc-capture.cpp） ----------
// 助手输出的是编码好的 H.264 码流，经 MessagePort 直接送到页面的 Worker，由它塞进 WebRTC 的发送端（见 public/desktop-vworker.js）。
// 预览是另一路小尺寸的共享纹理，走 Electron 的 sharedTexture 接口变成页面里的 VideoFrame。
const CAPTURE = app.isPackaged ? path.join(process.resourcesPath, 'bin', 'lc-capture.exe') : path.join(__dirname, 'bin', 'lc-capture.exe');
let video = null; // { proc, info, handles, port }
function stopVideo() {
  const v = video; video = null;
  if (!v) return;
  try { v.proc.stdin.write('quit\n'); } catch {}
  setTimeout(() => { try { v.proc.kill(); } catch {} }, 1500);
  try { v.port.close(); } catch {}
}
function videoCmd(line) { if (video) { try { video.proc.stdin.write(line + '\n'); } catch {} } }
// o: { sourceId 或 test:true, fps, maxw, maxh, bitrate, cursor, preview }
function startVideo(o) {
  stopVideo();
  const args = [String(process.pid)];
  if (o.test) args.push('test');
  else {
    const id = String(o.sourceId || ''); const m = /^(screen|window):(\d+):/.exec(id);
    if (!m) throw new Error('bad source');
    if (m[1] === 'window') args.push('window', m[2]);
    else {
      // 屏幕：把这块屏幕中心点的物理坐标给助手，它自己找对应的显示器
      const known = knownSources.get(id); const displays = screen.getAllDisplays();
      const d = displays.find((x) => String(x.id) === String(known?.display_id)) || screen.getPrimaryDisplay();
      const r = screen.dipToScreenRect(null, d.bounds);
      args.push('monitor', String(Math.round(r.x + r.width / 2)), String(Math.round(r.y + r.height / 2)));
    }
  }
  for (const k of ['fps', 'maxw', 'maxh', 'bitrate', 'cursor', 'preview', 'pvw', 'lobitrate', 'lomaxh', 'lofps']) if (o[k] != null && Number.isFinite(Number(o[k]))) args.push('--' + k, String(Math.round(Number(o[k]))));
  const proc = spawn(CAPTURE, args, { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  const { port1, port2 } = new MessageChannelMain();
  const me = video = { proc, info: null, handles: [], port: port1, pv: 0, pvFail: 0 };
  // Worker 那头要关键帧（有观众进来 / 丢包）时直接从这个端口说，不绕页面主线程
  port1.on('message', (e) => { if (video === me && e.data && e.data.key) videoCmd(e.data.l ? 'keylo' : 'key'); });
  port1.start();
  const release = (k) => { if (video === me) { try { proc.stdin.write('R ' + k + '\n'); } catch {} } };
  return new Promise((resolve, reject) => {
    let buf = Buffer.alloc(0), settled = false;
    const preview = (k, ts) => {
      if (video !== me || !win || win.isDestroyed() || !me.info || !me.handles[k]) return release(k);
      let imported;
      try {
        imported = sharedTexture.importSharedTexture({
          textureInfo: { pixelFormat: 'bgra', codedSize: { width: me.info.pw, height: me.info.ph }, visibleRect: { x: 0, y: 0, width: me.info.pw, height: me.info.ph }, handle: { ntHandle: me.handles[k] }, timestamp: ts },
          allReferencesReleased: () => release(k),
        });
      } catch (e) { me.pvFail++; return release(k); }
      me.pv++;
      sharedTexture.sendSharedTexture({ frame: win.webContents.mainFrame, importedSharedTexture: imported }, ts).catch(() => { me.pvFail++; }).finally(() => { try { imported.release(); } catch {} });
    };
    const onEvent = (m) => {
      if (m.ev === 'init' || m.ev === 'size') {
        me.info = m; me.handles = (m.handles || []).map((h) => { const b = Buffer.alloc(8); b.writeBigUInt64LE(BigInt(h)); return b; });
        const info = { w: m.w, h: m.h, srcw: m.srcw, srch: m.srch, fps: m.fps, hdr: !!m.hdr, white: m.white, enc: m.enc, pw: m.pw, ph: m.ph, low: m.low || 0, loh: m.loh || 0, lofps: m.lofps || 0 };
        if (!settled) { settled = true; resolve(info); win.webContents.postMessage('lc:video-port', null, [port2]); }
        else send('lc:video-state', { state: 'size', ...info });
      } else if (m.ev === 'stats') send('lc:video-state', { state: 'stats', ...m });
      else if (m.ev === 'error') { if (!settled) { settled = true; reject(new Error(m.msg)); } else send('lc:video-state', { state: 'error', detail: m.msg }); }
      else if (m.ev === 'closed') send('lc:video-state', { state: 'closed' });
    };
    proc.stdout.on('data', (d) => {
      buf = buf.length ? Buffer.concat([buf, d]) : d;
      while (buf.length >= 16) {
        const len = buf.readUInt32LE(0); if (buf.length < 16 + len) break;
        const type = buf[4], flags = buf[5]; const ts = Number(buf.readBigUInt64LE(8));
        if (type === 1) { if (video === me) port1.postMessage({ t: ts, k: flags & 1, l: (flags >> 1) & 1, d: new Uint8Array(buf.subarray(16, 16 + len)) }); } // l：0 主画面 1 低档。拷一份：底下的缓冲马上要复用
        else if (type === 2) { try { onEvent(JSON.parse(buf.toString('utf8', 16, 16 + len))); } catch {} }
        else if (type === 3) preview(flags, ts);
        buf = buf.subarray(16 + len);
      }
      if (!buf.length) buf = Buffer.alloc(0);
    });
    proc.stderr.on('data', () => {});
    proc.on('error', (e) => { if (video === me) video = null; if (!settled) { settled = true; reject(e); } });
    proc.on('exit', (code) => {
      if (!settled) { settled = true; reject(new Error('采集助手退出（' + code + '）')); }
      if (video === me) { video = null; try { port1.close(); } catch {} send('lc:video-state', { state: 'exit', code }); }
    });
  });
}

// ---------- 页面接口（只认本站页面的主框架） ----------
function handle(ch, fn) {
  ipcMain.handle(ch, (e, ...a) => {
    const f = e.senderFrame;
    if (!win || e.sender !== win.webContents || !f || f.parent || !fromApp(f.url)) throw new Error('forbidden');
    return fn(...a);
  });
}
handle('lc:sources', (opts) => listSources(opts));
handle('lc:pick-source', (id) => { pendingSource = typeof id === 'string' ? id : null; return true; });
handle('lc:audio-apps', () => listAudioApps());
handle('lc:audio-start', (o) => { startAudio({ mode: String(o?.mode || 'all'), pid: Number(o?.pid) }); return true; });
handle('lc:audio-stop', () => { stopAudio(); return true; });
handle('lc:video-start', (o) => startVideo(o || {}));
handle('lc:video-stop', () => { stopVideo(); return true; });
handle('lc:video-bitrate', (b, lo) => { const v = Math.round(Number(b)); if (v >= 100000 && v <= 200000000) videoCmd('br ' + v); const l = Math.round(Number(lo)); if (l >= 100000 && l <= 50000000) videoCmd('brlo ' + l); return true; });
handle('lc:video-key', () => { videoCmd('key'); return true; });
handle('lc:video-preview', (on) => { videoCmd('pv ' + (on ? 1 : 0)); return true; });
handle('lc:set-live', (on) => { setLive(on); return true; });
handle('lc:win-state', () => winState());
handle('lc:metrics', () => app.getAppMetrics().map((m) => ({ pid: m.pid, type: m.type, name: m.name || m.serviceName || '', cpu: m.cpu.percentCPUUsage, mem: m.memory.workingSetSize, priv: m.memory.privateBytes || 0 })));
handle('lc:info', () => ({ version: app.getVersion(), electron: process.versions.electron, chrome: process.versions.chrome, url: APP_URL, packaged: app.isPackaged, hwAccel: cfg.hwAccel }));
handle('lc:get-cfg', () => ({ hwAccel: cfg.hwAccel }));
handle('lc:set-cfg', (o) => { if (typeof o?.hwAccel === 'boolean') { cfg.hwAccel = o.hwAccel; saveCfg(); } return { hwAccel: cfg.hwAccel }; });
handle('lc:relaunch', () => { setTimeout(() => { app.relaunch(); app.exit(0); }, 200); return true; });
// 断网页（本地文件）只有这一个入口
ipcMain.handle('lc:retry', (e) => { if (win && e.sender === win.webContents) win.loadURL(APP_URL); });

app.on('second-instance', () => { if (win && !HIDDEN) { if (win.isMinimized()) win.restore(); win.show(); win.focus(); } });
app.on('window-all-closed', () => app.quit());
app.on('before-quit', () => { stopAudio(); stopVideo(); });
app.on('web-contents-created', (e, wc) => { wc.on('will-attach-webview', (ev) => ev.preventDefault()); });

app.whenReady().then(() => {
  Menu.setApplicationMenu(null);
  const ses = session.defaultSession;
  // 权限：只有本站页面能用摄像头 / 麦克风 / 屏幕采集 / 全屏 / 剪贴板；系统通知一律不给
  const ALLOW = new Set(['media', 'display-capture', 'fullscreen', 'clipboard-sanitized-write', 'clipboard-read', 'speaker-selection', 'window-management', 'pointerLock']);
  ses.setPermissionRequestHandler((wc, perm, cb, details) => cb(ALLOW.has(perm) && fromApp(details?.requestingUrl || wc?.getURL() || '')));
  ses.setPermissionCheckHandler((wc, perm, origin) => ALLOW.has(perm) && fromApp(origin || ''));
  ses.setDisplayMediaRequestHandler(async (request, callback) => {
    const id = pendingSource; pendingSource = null;
    try {
      if (!id || !fromApp(request.securityOrigin || request.frame?.url || '')) return callback(null);
      // 优先用刚才列表里的那一项，不再枚举一遍（游戏占满显卡时枚举一次要 1 秒多）
      let src = knownSources.get(id);
      if (!src) src = (await desktopCapturer.getSources({ types: [id.startsWith('screen:') ? 'screen' : 'window'], thumbnailSize: { width: 0, height: 0 } })).find((s) => s.id === id);
      callback(src ? { video: src } : null);
    } catch { try { callback(null); } catch {} }
  });
  createWindow();
});
