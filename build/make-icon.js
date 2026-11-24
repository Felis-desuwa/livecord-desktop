// 生成应用图标 build/icon.ico（和 icon.png）：蓝紫色圆角方块 + 白色播放三角 + 右上角红点。
// 纯 Node 实现，不依赖图像库：node build/make-icon.js
const fs = require('fs'); const path = require('path'); const zlib = require('zlib');

function render(N) {
  const px = Buffer.alloc(N * N * 4); const S = 4; // 每个像素 4×4 采样做抗锯齿
  const inRound = (x, y, x0, y0, x1, y1, r) => {
    const cx = Math.min(Math.max(x, x0 + r), x1 - r), cy = Math.min(Math.max(y, y0 + r), y1 - r);
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r;
  };
  const inTri = (x, y, a, b, c) => {
    const s = (p, q, r) => (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1]);
    const d1 = s([x, y], a, b), d2 = s([x, y], b, c), d3 = s([x, y], c, a);
    return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
  };
  for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) {
    let r = 0, g = 0, b = 0, a = 0;
    for (let sj = 0; sj < S; sj++) for (let si = 0; si < S; si++) {
      const x = (i + (si + 0.5) / S) / N, y = (j + (sj + 0.5) / S) / N; // 0..1
      let c = null;
      if (inRound(x, y, 0.06, 0.1, 0.9, 0.94, 0.2)) c = [0x58, 0x65, 0xf2];
      if (c && inTri(x, y, [0.37, 0.33], [0.37, 0.71], [0.68, 0.52])) c = [255, 255, 255];
      const dd = (x - 0.8) ** 2 + (y - 0.2) ** 2;
      if (dd <= 0.17 ** 2) c = dd <= 0.125 ** 2 ? [0xed, 0x42, 0x45] : (c ? [0x1e, 0x1f, 0x22] : null); // 红点 + 一圈深色描边
      if (c) { r += c[0]; g += c[1]; b += c[2]; a += 255; }
    }
    const n = S * S, o = (j * N + i) * 4, cov = a / 255;
    px[o] = cov ? Math.round(r / cov) : 0; px[o + 1] = cov ? Math.round(g / cov) : 0; px[o + 2] = cov ? Math.round(b / cov) : 0; px[o + 3] = Math.round(a / n);
  }
  return px;
}
function png(N, px) {
  const crcT = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcT[n] = c >>> 0; }
  const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(N, 0); ihdr.writeUInt32BE(N, 4); ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc(N * (N * 4 + 1)); for (let y = 0; y < N; y++) px.copy(raw, y * (N * 4 + 1) + 1, y * N * 4, (y + 1) * N * 4);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
const sizes = [16, 24, 32, 48, 64, 128, 256]; const imgs = sizes.map((n) => png(n, render(n)));
const head = Buffer.alloc(6); head.writeUInt16LE(1, 2); head.writeUInt16LE(sizes.length, 4);
let off = 6 + 16 * sizes.length; const dir = [];
sizes.forEach((n, i) => { const e = Buffer.alloc(16); e[0] = n === 256 ? 0 : n; e[1] = n === 256 ? 0 : n; e.writeUInt16LE(1, 4); e.writeUInt16LE(32, 6); e.writeUInt32LE(imgs[i].length, 8); e.writeUInt32LE(off, 12); off += imgs[i].length; dir.push(e); });
fs.writeFileSync(path.join(__dirname, 'icon.ico'), Buffer.concat([head, ...dir, ...imgs]));
fs.writeFileSync(path.join(__dirname, 'icon.png'), imgs[imgs.length - 1]);
console.log('icon written', sizes.join(','));
