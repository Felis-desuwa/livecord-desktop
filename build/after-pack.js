// 打包后删掉用不到的库，减小安装包。
//  - dxcompiler.dll / dxil.dll：WebGPU 的着色器编译器，这个软件不用 WebGPU
//  - vk_swiftshader*.、vulkan-1.dll：Vulkan 和它的软件渲染实现，Windows 上 Chromium 默认走 D3D11（ANGLE），用不到
// d3dcompiler_47.dll（ANGLE 要用）和 ffmpeg.dll 不能删。
const fs = require('fs'); const path = require('path');
const DROP = ['dxcompiler.dll', 'dxil.dll', 'vk_swiftshader.dll', 'vk_swiftshader_icd.json', 'vulkan-1.dll'];
exports.default = async function afterPack(ctx) {
  let saved = 0;
  for (const f of DROP) {
    const p = path.join(ctx.appOutDir, f);
    try { saved += fs.statSync(p).size; fs.rmSync(p); } catch {}
  }
  console.log(`  • after-pack: 删掉多余的库，省 ${(saved / 1048576).toFixed(1)} MB`);
};
