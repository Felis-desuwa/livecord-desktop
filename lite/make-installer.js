// 打「可选版本」的安装器：node lite/make-installer.js   （先跑过 native\build.cmd 和 lite\build.cmd）
// 用的是 electron-builder 下载到本机缓存里的 NSIS 和 INetC 插件（打过一次完整版安装包就有了）。
// 输出 dist/LiveCord-Installer.exe
const fs = require('fs'); const path = require('path'); const { spawnSync } = require('child_process');
const root = path.resolve(__dirname, '..'); const version = require(path.join(root, 'package.json')).version;
const cache = path.join(process.env.LOCALAPPDATA || '', 'electron-builder', 'Cache');
function find(dir, name, depth = 4, want = null) {
  if (depth < 0 || !fs.existsSync(dir)) return null;
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isFile() && e.name.toLowerCase() === name.toLowerCase() && (!want || want.test(p))) return p;
    if (e.isDirectory()) { const r = find(p, name, depth - 1, want); if (r) return r; }
  }
  return null;
}
const makensis = find(cache, 'makensis.exe', 3, /nsis-[^\\/]*[\\/]makensis\.exe$/i); // 要顶层那个（Bin 里的那个找不到自己的 Stubs）
const inetc = find(cache, 'INetC.dll', 6, /x86-unicode/i);
if (!makensis || !inetc) { console.error('没找到 NSIS 或 INetC 插件：先在 desktop 目录跑一次 npm run dist，让 electron-builder 把它们下载下来'); process.exit(1); }
for (const f of ['lite/out/LiveCordLite.exe', 'bin/lc-audio.exe', 'bin/lc-capture.exe']) if (!fs.existsSync(path.join(root, f))) { console.error('缺文件：' + f); process.exit(1); }
const out = path.join(root, 'dist', 'LiveCord-Installer.exe'); fs.mkdirSync(path.dirname(out), { recursive: true });
const r = spawnSync(makensis, ['/V2', '/INPUTCHARSET', 'UTF8', '/DOUT=' + out, '/DINETC_DIR=' + path.dirname(inetc), '/DVERSION=' + version, path.join(__dirname, 'installer.nsi')], { stdio: 'inherit', cwd: __dirname });
if (r.status !== 0) process.exit(r.status || 1);
console.log(`OK ${out} ${(fs.statSync(out).size / 1024).toFixed(0)} KB`);
