'use strict';
// 预加载脚本：给网站页面一个很窄的接口 window.lcDesktop。页面拿不到 Node，也拿不到 ipcRenderer 本身
const { contextBridge, ipcRenderer } = require('electron');

const subs = { win: new Set(), audio: new Set(), video: new Set() };
ipcRenderer.on('lc:video-state', (e, s) => { for (const f of subs.video) try { f(s); } catch {} });
ipcRenderer.on('lc:win-state', (e, s) => { for (const f of subs.win) try { f(s); } catch {} });
ipcRenderer.on('lc:audio-state', (e, s) => { for (const f of subs.audio) try { f(s); } catch {} });
// 声音数据走 MessagePort：从主进程直接送进页面的 AudioWorklet，不经过页面主线程。
// contextBridge 不能传端口，用 window.postMessage 转交给页面
ipcRenderer.on('lc:audio-port', (e) => { window.postMessage({ lcAudioPort: true }, location.origin, e.ports); });
ipcRenderer.on('lc:video-port', (e) => { window.postMessage({ lcVideoPort: true }, location.origin, e.ports); });

// 原生采集的预览画面：主进程把共享纹理发过来，这里变成 VideoFrame 再转交给页面（VideoFrame 可以随 postMessage 转移）
let sharedTexture = null; try { sharedTexture = require('electron').sharedTexture; } catch {}
let texErr = sharedTexture && sharedTexture.setSharedTextureReceiver ? null : 'no-api';
if (!texErr) {
  try {
    sharedTexture.setSharedTextureReceiver(async (data) => {
      const imp = data.importedSharedTexture;
      try { const vf = imp.getVideoFrame(); window.postMessage({ lcFrame: vf }, location.origin, [vf]); }
      catch (e) { texErr = String(e && e.message || e); }
      finally { try { imp.release(); } catch {} }
    });
  } catch (e) { texErr = String(e && e.message || e); }
}

const call = (ch) => (...a) => ipcRenderer.invoke(ch, ...a);
const on = (set) => (fn) => { set.add(fn); return () => set.delete(fn); };

if (location.protocol === 'file:') {
  contextBridge.exposeInMainWorld('lcShell', { retry: call('lc:retry') });
} else {
  contextBridge.exposeInMainWorld('lcDesktop', {
    info: call('lc:info'),
    sources: call('lc:sources'),          // 列出屏幕和窗口（带缩略图）
    pickSource: call('lc:pick-source'),   // 选定来源；紧接着调用 getDisplayMedia 就会拿到它
    audioApps: call('lc:audio-apps'),     // 列出能采集声音的程序
    audioStart: call('lc:audio-start'),   // { mode: 'app' | 'except' | 'all', pid }
    audioStop: call('lc:audio-stop'),
    onAudioState: on(subs.audio),
    videoStart: call('lc:video-start'),   // { sourceId, fps, maxw, maxh, bitrate, preview }：原生采集 + 编码。码流走 lcVideoPort 端口，预览帧随 message 事件的 lcFrame 送达
    videoStop: call('lc:video-stop'),
    videoBitrate: call('lc:video-bitrate'),
    videoKey: call('lc:video-key'),
    videoPreview: call('lc:video-preview'),
    videoSupport: () => texErr,
    onVideoState: on(subs.video),
    setLive: call('lc:set-live'),
    winState: call('lc:win-state'),
    onWinState: on(subs.win),
    metrics: call('lc:metrics'),
    getCfg: call('lc:get-cfg'),
    setCfg: call('lc:set-cfg'),
    relaunch: call('lc:relaunch'),
  });
}
