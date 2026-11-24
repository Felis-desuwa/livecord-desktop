// lc-capture：LiveCord 桌面版的画面采集 + 编码助手（Windows 10 2004 及以上，推荐 Windows 11）
//
// 为什么不用内核（Chromium）自带的屏幕采集和编码：实测边打游戏边播 1440p 时，
//   - 自带的抓屏每帧都要把整幅画面读回内存，HDR 屏上一次 8～9 毫秒，且内核规定两次抓取的间隔不小于单次耗时的两倍 → 只有 50～60 帧
//   - 把显存里的画面直接交给内核的编码器，它每帧还要同步读回一次 → 约 84 帧，还占 1.7 个核
// 所以这里整条链路都在显卡上做完：
//   Windows Graphics Capture 拿到显存里的画面 → 着色器做缩放、HDR→SDR、转 NV12 → 显卡的 H.264 编码器（Media Foundation）
// 输出的是编码好的 H.264 码流（每秒几 MB），软件只负责发出去。预览另外输出一路小尺寸的共享纹理。
//
//   lc-capture <父进程号> monitor <x> <y> [选项]     采集包含屏幕坐标 (x,y) 的那块屏幕
//   lc-capture <父进程号> window <HWND> [选项]       采集一个窗口
//   lc-capture <父进程号> test [选项]                不采集，编码测试画面（验证链路用）
//   选项：--fps N --maxw W --maxh H（输出不超过这个尺寸，等比缩小） --bitrate BPS --cursor 0|1 --preview 0|1 --pvw W
//
// 和父进程的约定：
//   标准输出：一条条消息，头 16 字节 [u32 长度][u8 类型][u8 标志][u16 保留][u64 时间戳微秒]，后面是内容
//     类型 1  一帧 H.264（Annex-B），标志 bit0 = 关键帧
//     类型 2  JSON 事件：init / stats / size / closed / error
//     类型 3  预览帧写好了，标志 = 共享纹理槽位
//   标准输入（一行一条）：br <bps> 改码率 / key 要关键帧 / pv 0|1 开关预览 / R <槽位> 预览纹理用完了 / quit
//   标准输入被关闭（父进程退出）时自动结束。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
using winrt::com_ptr;

// ---------------- 输出 ----------------
static std::mutex outMu;
static void emit(BYTE type, BYTE flags, ULONGLONG ts, const void* data, DWORD len) {
  BYTE hdr[16]; memcpy(hdr, &len, 4); hdr[4] = type; hdr[5] = flags; hdr[6] = hdr[7] = 0; memcpy(hdr + 8, &ts, 8);
  std::lock_guard<std::mutex> g(outMu);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE); DWORD wr;
  if (!WriteFile(out, hdr, 16, &wr, nullptr)) ExitProcess(0);
  if (len && !WriteFile(out, data, len, &wr, nullptr)) ExitProcess(0);
}
static void event(const char* fmt, ...) {
  char buf[4096]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap); va_end(ap);
  if (n > 0) emit(2, 0, 0, buf, (DWORD)n);
}
static int die(const char* what, HRESULT hr = 0) { event("{\"ev\":\"error\",\"msg\":\"%s 0x%08lx\"}", what, (unsigned long)hr); Sleep(50); return 2; }
static double nowSec() { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
static std::string narrow(const wchar_t* w) { std::string s; for (; *w; w++) s += (*w < 128 && *w != '"' && *w != '\\') ? (char)*w : '?'; return s; }

struct Args { DWORD parent = 0; std::string mode; long long t1 = 0, t2 = 0; int fps = 60, maxw = 0, maxh = 0, bitrate = 8000000, cursor = 1, preview = 1, pvw = 960, lobitrate = 0, lomaxh = 720, lofps = 30; };
static Args A;
static std::atomic<int> gBitrate{0}, gBitrateLo{0};
static std::atomic<bool> gQuit{false}, gPreview{true};

// ---------------- 显卡设备 ----------------
static com_ptr<ID3D11Device> dev; static com_ptr<ID3D11DeviceContext> ctx; static LUID adapterLuid{};
static std::mutex gpuMu; // 立即上下文不是线程安全的：采集回调、定时补帧、编码都要排队用

// ---------------- 预览用的共享纹理池 ----------------
struct Slot { com_ptr<ID3D11Texture2D> tex; com_ptr<ID3D11RenderTargetView> rtv; HANDLE local = nullptr, remote = nullptr; std::atomic<int> busy{0}; double sentAt = 0; };
struct Pool {
  std::vector<Slot> slots; int w = 0, h = 0, next = 0; HANDLE parent = nullptr;
  HRESULT create(HANDLE par, int n, int W, int H) {
    destroy(); parent = par; slots = std::vector<Slot>(n); w = W; h = H;
    for (auto& s : slots) {
      D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
      HRESULT hr = dev->CreateTexture2D(&d, nullptr, s.tex.put()); if (FAILED(hr)) return hr;
      hr = dev->CreateRenderTargetView(s.tex.get(), nullptr, s.rtv.put()); if (FAILED(hr)) return hr;
      auto r = s.tex.as<IDXGIResource1>();
      hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &s.local); if (FAILED(hr)) return hr;
      if (!DuplicateHandle(GetCurrentProcess(), s.local, parent, &s.remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
  }
  // 把复制进父进程的句柄关掉，否则纹理的显存一直被占着
  void destroy() { for (auto& s : slots) { if (s.remote && parent) DuplicateHandle(parent, s.remote, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE); if (s.local) CloseHandle(s.local); } slots.clear(); }
  int acquire() {
    int n = (int)slots.size(); double t = nowSec();
    for (int i = 0; i < n; i++) { int k = (next + i) % n; int exp = 0; if (slots[k].busy.compare_exchange_strong(exp, 1)) { next = (k + 1) % n; return k; } }
    for (int i = 0; i < n; i++) if (t - slots[i].sentAt > 2.0) { next = (i + 1) % n; return i; } // 2 秒没还的当作丢了
    return -1;
  }
  std::string handlesJson() const { std::string s = "["; for (size_t i = 0; i < slots.size(); i++) { if (i) s += ","; s += std::to_string((unsigned long long)(UINT_PTR)slots[i].remote); } return s + "]"; }
};
static Pool pvPool;

// ---------------- 着色器：缩放 + HDR→SDR + 转 NV12 ----------------
static const char* HLSL = R"(
Texture2D src : register(t0); SamplerState smp : register(s0);
cbuffer P : register(b0) { float hdr; float whiteScale; float2 pad; };
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) { VO o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float3 toSdr(float2 uv) {
  float3 c = src.Sample(smp, uv).rgb;
  if (hdr > 0.5) {
    // HDR 桌面给的是线性 scRGB（1.0 = 80 尼特）。先按系统的"SDR 内容亮度"换算，让普通内容的白色落在 1.0；
    // 高光超出的部分按最大通道平滑压缩（保色相），最后转成 sRGB 的非线性值
    c = max(c, 0.0) * whiteScale;
    float m = max(c.r, max(c.g, c.b));
    const float k = 0.8;
    if (m > k) { float t = k + (1.0 - k) * (1.0 - exp(-(m - k) / (1.0 - k))); c *= t / m; }
    c = saturate(c);
    c = lerp(c * 12.92, 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055, step(0.0031308, c));
  }
  return saturate(c);
}
// BT.709 有限范围
float4 psY(VO i) : SV_Target { float3 c = toSdr(i.uv); float y = dot(c, float3(0.2126, 0.7152, 0.0722)); return float4(16.0 / 255.0 + y * 219.0 / 255.0, 0, 0, 1); }
float4 psUV(VO i) : SV_Target { float3 c = toSdr(i.uv);
  float u = dot(c, float3(-0.114572, -0.385428, 0.5)), v = dot(c, float3(0.5, -0.454153, -0.045847));
  return float4(128.0 / 255.0 + u * 224.0 / 255.0, 128.0 / 255.0 + v * 224.0 / 255.0, 0, 1); }
float4 psRGB(VO i) : SV_Target { return float4(toSdr(i.uv), 1); }
)";
struct Shaders {
  com_ptr<ID3D11VertexShader> vs; com_ptr<ID3D11PixelShader> psY, psUV, psRGB; com_ptr<ID3D11SamplerState> smp; com_ptr<ID3D11Buffer> cb;
  HRESULT init() {
    auto comp = [](const char* entry, const char* target, ID3DBlob** out) { com_ptr<ID3DBlob> err; return D3DCompile(HLSL, strlen(HLSL), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, err.put()); };
    com_ptr<ID3DBlob> b; HRESULT hr;
    if (FAILED(hr = comp("vs", "vs_5_0", b.put()))) return hr; if (FAILED(hr = dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, vs.put()))) return hr; b = nullptr;
    if (FAILED(hr = comp("psY", "ps_5_0", b.put()))) return hr; if (FAILED(hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, psY.put()))) return hr; b = nullptr;
    if (FAILED(hr = comp("psUV", "ps_5_0", b.put()))) return hr; if (FAILED(hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, psUV.put()))) return hr; b = nullptr;
    if (FAILED(hr = comp("psRGB", "ps_5_0", b.put()))) return hr; if (FAILED(hr = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, psRGB.put()))) return hr;
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(hr = dev->CreateSamplerState(&sd, smp.put()))) return hr;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return dev->CreateBuffer(&bd, nullptr, cb.put());
  }
  void pass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, int w, int h, ID3D11ShaderResourceView* srv) {
    D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 }; ID3D11RenderTargetView* r = rtv; ID3D11SamplerState* s = smp.get(); ID3D11Buffer* c = cb.get();
    ctx->OMSetRenderTargets(1, &r, nullptr); ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs.get(), nullptr, 0); ctx->PSSetShader(ps, nullptr, 0); ctx->PSSetShaderResources(0, 1, &srv); ctx->PSSetSamplers(0, 1, &s); ctx->PSSetConstantBuffers(0, 1, &c);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul = nullptr; ctx->PSSetShaderResources(0, 1, &nul); ID3D11RenderTargetView* nr = nullptr; ctx->OMSetRenderTargets(1, &nr, nullptr);
  }
};
static Shaders sh;

// ---------------- 编码器（Media Foundation 的硬件 H.264 编码器，异步） ----------------
struct Encoder {
  com_ptr<IMFTransform> mft; com_ptr<IMFMediaEventGenerator> evg; com_ptr<ICodecAPI> codec; com_ptr<IMFDXGIDeviceManager> mgr; UINT token = 0;
  std::mutex mu; std::atomic<int> need{0}; std::atomic<bool> stop{false}; HANDLE thread = nullptr; bool providesSamples = true; DWORD outSize = 0;
  int w = 0, h = 0, fps = 60; std::string name; BYTE layer = 0; std::atomic<bool>* keyFlag = nullptr;
  // 统计
  std::atomic<long long> encFrames{0}, encBytes{0}, busyDrops{0}; std::atomic<long long> latSumUs{0}, latN{0};
  struct Pending { LONGLONG ts; double at; }; std::vector<Pending> pending; std::mutex pmu;

  void setU32(const GUID& g, ULONG v) { if (!codec) return; VARIANT var; VariantInit(&var); var.vt = VT_UI4; var.ulVal = v; codec->SetValue(&g, &var); }
  void setBool(const GUID& g, bool v) { if (!codec) return; VARIANT var; VariantInit(&var); var.vt = VT_BOOL; var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; codec->SetValue(&g, &var); }
  void applyBitrate(int bps) { setU32(CODECAPI_AVEncCommonMeanBitRate, bps); setU32(CODECAPI_AVEncCommonMaxBitRate, bps + bps / 5); }

  HRESULT init(int W, int H, int FPS, int bps) {
    w = W; h = H; fps = FPS; HRESULT hr;
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 }, out{ MFMediaType_Video, MFVideoFormat_H264 };
    com_ptr<IMFAttributes> ea; MFCreateAttributes(ea.put(), 1); ea->SetBlob(MFT_ENUM_ADAPTER_LUID, (const UINT8*)&adapterLuid, sizeof(LUID));
    IMFActivate** acts = nullptr; UINT32 n = 0;
    hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, ea.get(), &acts, &n);
    if (FAILED(hr) || !n) return FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND;
    wchar_t nm[256] = L"?"; UINT32 nl = 0; acts[0]->GetString(MFT_FRIENDLY_NAME_Attribute, nm, 256, &nl); name = narrow(nm);
    hr = acts[0]->ActivateObject(IID_PPV_ARGS(mft.put()));
    for (UINT32 i = 0; i < n; i++) acts[i]->Release(); CoTaskMemFree(acts);
    if (FAILED(hr)) return hr;
    com_ptr<IMFAttributes> attrs; if (FAILED(hr = mft->GetAttributes(attrs.put()))) return hr;
    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE); attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (FAILED(hr = MFCreateDXGIDeviceManager(&token, mgr.put()))) return hr;
    if (FAILED(hr = mgr->ResetDevice(dev.get(), token))) return hr;
    if (FAILED(hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr.get()))) return hr;
    mft.try_as(codec);
    DWORD ni = 0, no = 0; mft->GetStreamCount(&ni, &no); DWORD inId = 0, outId = 0; if (mft->GetStreamIDs(1, &inId, 1, &outId) == E_NOTIMPL) { inId = outId = 0; }
    // 码率控制等要在设媒体类型之前设一遍（有的驱动只在这时认），之后再设一遍
    auto modes = [&]() {
      setU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
      applyBitrate(bps);
      setBool(CODECAPI_AVLowLatencyMode, true);
      setU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
      setU32(CODECAPI_AVEncMPVGOPSize, 0x7fffffff); // 不定期出关键帧，只在有人要的时候出（key 命令）
      setU32(CODECAPI_AVEncCommonQualityVsSpeed, 50);
    };
    modes();
    com_ptr<IMFMediaType> ot; MFCreateMediaType(ot.put());
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    ot->SetUINT32(MF_MT_AVG_BITRATE, bps); MFSetAttributeRatio(ot.get(), MF_MT_FRAME_RATE, fps, 1); MFSetAttributeSize(ot.get(), MF_MT_FRAME_SIZE, w, h);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeRatio(ot.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    ot->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235); ot->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    ot->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709); ot->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (FAILED(hr = mft->SetOutputType(outId, ot.get(), 0))) return hr;
    com_ptr<IMFMediaType> it; MFCreateMediaType(it.put());
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeRatio(it.get(), MF_MT_FRAME_RATE, fps, 1); MFSetAttributeSize(it.get(), MF_MT_FRAME_SIZE, w, h);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); MFSetAttributeRatio(it.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    it->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235); it->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (FAILED(hr = mft->SetInputType(inId, it.get(), 0))) return hr;
    modes();
    MFT_OUTPUT_STREAM_INFO si{}; mft->GetOutputStreamInfo(0, &si);
    providesSamples = !!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)); outSize = si.cbSize ? si.cbSize : (DWORD)(w * h * 2);
    mft.try_as(evg); if (!evg) return E_NOINTERFACE;
    mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (FAILED(hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return hr;
    if (FAILED(hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return hr;
    thread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { ((Encoder*)p)->loop(); return 0; }, this, 0, nullptr);
    return S_OK;
  }
  // 交一帧 NV12 纹理给编码器。编码器还没要输入时先放着（只留最新的一帧），它一要就给；两帧挨得很近到达时不至于丢
  struct Waiting { com_ptr<ID3D11Texture2D> tex; LONGLONG ts = 0; bool key = false; bool has = false; } waiting; std::mutex wmu;
  bool submit(ID3D11Texture2D* nv12, LONGLONG ts100, bool key) {
    {
      std::lock_guard<std::mutex> g(wmu);
      if (need.load() <= 0 || waiting.has) {
        if (waiting.has) { busyDrops++; key = key || waiting.key; }
        waiting.tex.copy_from(nv12); waiting.ts = ts100; waiting.key = key; waiting.has = true;
        return true;
      }
    }
    return feed(nv12, ts100, key);
  }
  void drainWaiting() {
    com_ptr<ID3D11Texture2D> t; LONGLONG ts = 0; bool key = false;
    { std::lock_guard<std::mutex> g(wmu); if (!waiting.has || need.load() <= 0) return; t = waiting.tex; ts = waiting.ts; key = waiting.key; waiting.has = false; waiting.tex = nullptr; }
    if (!feed(t.get(), ts, key) && key && keyFlag) keyFlag->store(true);
  }
  bool feed(ID3D11Texture2D* nv12, LONGLONG ts100, bool key) {
    int exp = need.load(); do { if (exp <= 0) { busyDrops++; return false; } } while (!need.compare_exchange_weak(exp, exp - 1));
    com_ptr<IMFMediaBuffer> buf; com_ptr<IMFSample> smp;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, buf.put())) || FAILED(MFCreateSample(smp.put()))) { need++; return false; }
    DWORD len = (DWORD)(w * h * 3 / 2); buf->SetCurrentLength(len);
    smp->AddBuffer(buf.get()); smp->SetSampleTime(ts100); smp->SetSampleDuration(10000000LL / fps);
    { std::lock_guard<std::mutex> g(pmu); if (pending.size() > 64) pending.erase(pending.begin()); pending.push_back({ ts100, nowSec() }); }
    std::lock_guard<std::mutex> g(mu);
    if (key) setU32(CODECAPI_AVEncVideoForceKeyFrame, 1);
    HRESULT hr = mft->ProcessInput(0, smp.get(), 0);
    if (FAILED(hr)) { if (hr != MF_E_NOTACCEPTING) { need++; event("{\"ev\":\"warn\",\"msg\":\"ProcessInput 0x%08lx\"}", (unsigned long)hr); } return false; }
    return true;
  }
  void output() {
    MFT_OUTPUT_DATA_BUFFER ob{}; DWORD status = 0; com_ptr<IMFSample> own;
    HRESULT hr;
    {
      std::lock_guard<std::mutex> g(mu);
      if (!providesSamples) { com_ptr<IMFMediaBuffer> b; MFCreateMemoryBuffer(outSize, b.put()); MFCreateSample(own.put()); own->AddBuffer(b.get()); ob.pSample = own.get(); }
      hr = mft->ProcessOutput(0, 1, &ob, &status);
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { com_ptr<IMFMediaType> t; if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, t.put()))) mft->SetOutputType(0, t.get(), 0); return; }
    }
    if (ob.pEvents) ob.pEvents->Release();
    if (FAILED(hr) || !ob.pSample) return;
    com_ptr<IMFSample> s; if (providesSamples) s.attach(ob.pSample); else s = own;
    LONGLONG ts = 0; s->GetSampleTime(&ts);
    UINT32 clean = 0; s->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    com_ptr<IMFMediaBuffer> buf; if (FAILED(s->ConvertToContiguousBuffer(buf.put()))) return;
    BYTE* p = nullptr; DWORD len = 0; if (FAILED(buf->Lock(&p, nullptr, &len))) return;
    emit(1, (BYTE)((clean ? 1 : 0) | (layer << 1)), (ULONGLONG)(ts / 10), p, len);
    buf->Unlock();
    encFrames++; encBytes += len;
    { std::lock_guard<std::mutex> g(pmu); for (size_t i = 0; i < pending.size(); i++) if (pending[i].ts == ts) { latSumUs += (long long)((nowSec() - pending[i].at) * 1e6); latN++; pending.erase(pending.begin(), pending.begin() + i + 1); break; } }
  }
  void loop() {
    while (!stop.load()) {
      com_ptr<IMFMediaEvent> ev; HRESULT hr = evg->GetEvent(0, ev.put());
      if (FAILED(hr)) { if (!stop.load()) event("{\"ev\":\"error\",\"msg\":\"encoder event 0x%08lx\"}", (unsigned long)hr); break; }
      MediaEventType t = MEUnknown; ev->GetType(&t);
      if (t == METransformNeedInput) { need++; drainWaiting(); }
      else if (t == METransformHaveOutput) output();
    }
  }
  void shutdown() {
    stop.store(true);
    if (mft) { std::lock_guard<std::mutex> g(mu); mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); }
    com_ptr<IMFShutdown> sd; if (mft) mft.try_as(sd); if (sd) sd->Shutdown();
    if (thread) { WaitForSingleObject(thread, 1000); CloseHandle(thread); thread = nullptr; }
    mft = nullptr; evg = nullptr; codec = nullptr;
  }
};

// ---------------- 管线：源纹理 → NV12 → 编码器（+ 预览） ----------------
// 主画面一路；可选再出一路低档（--lobitrate > 0）：分辨率和帧率都低，给下行带宽不够的观众，服务器按每个观众的网络自动选
struct Layer {
  Encoder* enc = nullptr; int w = 0, h = 0, fps = 60; BYTE id = 0;
  static const int RING = 8;
  com_ptr<ID3D11Texture2D> nv12[RING]; com_ptr<ID3D11RenderTargetView> rtvY[RING], rtvUV[RING]; int ring = 0, last = -1;
  double nextDue = 0, lastSubmit = 0; LONGLONG lastTs = 0; std::atomic<bool> forceKey{true};
  HRESULT init(int W, int H, int FPS, int bps, BYTE layerId) {
    w = W; h = H; fps = FPS; id = layerId; ring = 0; last = -1; nextDue = 0; lastSubmit = 0; lastTs = 0; forceKey.store(true); HRESULT hr;
    for (int i = 0; i < RING; i++) {
      D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_NV12; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      nv12[i] = nullptr; rtvY[i] = nullptr; rtvUV[i] = nullptr;
      if (FAILED(hr = dev->CreateTexture2D(&d, nullptr, nv12[i].put()))) return hr;
      D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Format = DXGI_FORMAT_R8_UNORM;
      if (FAILED(hr = dev->CreateRenderTargetView(nv12[i].get(), &rd, rtvY[i].put()))) return hr;
      rd.Format = DXGI_FORMAT_R8G8_UNORM; if (FAILED(hr = dev->CreateRenderTargetView(nv12[i].get(), &rd, rtvUV[i].put()))) return hr;
    }
    enc = new Encoder(); enc->layer = id; enc->keyFlag = &forceKey;
    return enc->init(w, h, fps, bps);
  }
  void destroy() { if (enc) { enc->shutdown(); delete enc; enc = nullptr; } }
  // 按这一路的帧率挑帧；要这一帧就画进 NV12 并交给编码器。调用方已经拿着显卡锁
  bool take(double t) { double period = 1.0 / fps; if (t + period * 0.25 < nextDue) return false; nextDue = std::max(nextDue + period, t - period * 0.5); return true; }
  void draw(ID3D11ShaderResourceView* srv) {
    int k = ring; ring = (ring + 1) % RING;
    sh.pass(sh.psY.get(), rtvY[k].get(), w, h, srv);
    sh.pass(sh.psUV.get(), rtvUV[k].get(), w / 2, h / 2, srv);
    last = k;
  }
  void submit(double t) {
    lastSubmit = t; lastTs = std::max(lastTs + 1, (LONGLONG)(t * 1e7));
    bool key = forceKey.exchange(false);
    if (!enc->submit(nv12[last].get(), lastTs, key) && key) forceKey.store(true);
  }
  // 画面没变时系统不给新帧：每半秒把上一帧再编一次（很小的 P 帧），保持连接活着；有人要关键帧时也靠它立刻出
  void tick(double t) {
    if (last < 0 || !enc) return;
    if (t - lastSubmit < (forceKey.load() ? 0.03 : 0.5)) return;
    submit(t);
  }
};
struct Pipeline {
  Layer hi, lo; bool hasLo = false; Encoder* enc = nullptr; // enc = hi.enc（给外面读统计用）
  int srcW = 0, srcH = 0, outW = 0, outH = 0, pvW = 0, pvH = 0; bool hdr = false; float whiteScale = 1;
  double t0 = 0, lastPv = 0;
  std::atomic<long long> arrived{0}, paced{0};

  HRESULT init(int sw, int sh, bool isHdr, float ws, HANDLE parent) {
    srcW = sw; srcH = sh; hdr = isHdr; whiteScale = ws;
    double sc = 1; if (A.maxw > 0) sc = std::min(sc, (double)A.maxw / sw); if (A.maxh > 0) sc = std::min(sc, (double)A.maxh / sh);
    outW = std::max(64, (int)(sw * sc + 0.5) & ~1); outH = std::max(64, (int)(sh * sc + 0.5) & ~1);
    HRESULT hr;
    if (FAILED(hr = hi.init(outW, outH, A.fps, gBitrate.load(), 0))) return hr;
    enc = hi.enc;
    // 低档：比主画面小才有意义
    hasLo = false;
    if (A.lobitrate > 0 && outH > A.lomaxh) {
      double ls = (double)A.lomaxh / outH; int lw = std::max(64, (int)(outW * ls + 0.5) & ~1), lh = std::max(64, (int)(outH * ls + 0.5) & ~1);
      if (SUCCEEDED(lo.init(lw, lh, std::min(A.lofps, A.fps), gBitrateLo.load(), 1))) hasLo = true; else lo.destroy();
    }
    double ps = std::min(1.0, (double)A.pvw / outW); pvW = std::max(64, (int)(outW * ps + 0.5) & ~1); pvH = std::max(36, (int)(outH * ps + 0.5) & ~1);
    if (FAILED(hr = pvPool.create(parent, 4, pvW, pvH))) return hr;
    t0 = nowSec(); lastPv = 0;
    return S_OK;
  }
  void destroy() { hi.destroy(); lo.destroy(); enc = nullptr; hasLo = false; pvPool.destroy(); }
  void setConsts() { float c[4] = { hdr ? 1.f : 0.f, whiteScale, 0, 0 }; ctx->UpdateSubresource(sh.cb.get(), 0, nullptr, c, 0, 0); }

  // 来了一帧新画面。按目标帧率挑帧：屏幕 240Hz、目标 120 帧时每两帧取一帧
  void onFrame(ID3D11Texture2D* src) {
    arrived++;
    double t = nowSec() - t0;
    std::lock_guard<std::mutex> g(gpuMu);
    if (!hi.enc) return;
    bool wantHi = hi.take(t), wantLo = hasLo && lo.take(t);
    bool wantPv = gPreview.load() && t - lastPv >= 1.0 / 30 - 0.002;
    if (!wantHi && !wantLo) return; // 预览跟着主画面的节奏出，不单独抓
    if (wantHi) paced++;
    com_ptr<ID3D11ShaderResourceView> srv; if (FAILED(dev->CreateShaderResourceView(src, nullptr, srv.put()))) return;
    setConsts();
    if (wantHi) hi.draw(srv.get());
    if (wantLo) lo.draw(srv.get());
    int slot = -1;
    if (wantPv) { slot = pvPool.acquire(); if (slot >= 0) { sh.pass(sh.psRGB.get(), pvPool.slots[slot].rtv.get(), pvW, pvH, srv.get()); lastPv = t; } }
    ctx->Flush();
    if (wantHi) hi.submit(t);
    if (wantLo) lo.submit(t);
    if (slot >= 0) { pvPool.slots[slot].sentAt = nowSec(); emit(3, (BYTE)slot, (ULONGLONG)(t * 1e6), nullptr, 0); }
  }
  void tick() {
    std::lock_guard<std::mutex> g(gpuMu);
    double t = nowSec() - t0;
    hi.tick(t); if (hasLo) lo.tick(t);
  }
};
static Pipeline pl;

// ---------------- 标准输入 ----------------
static DWORD WINAPI stdinThread(LPVOID) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE); char buf[256]; std::string line; DWORD n = 0;
  while (ReadFile(in, buf, sizeof(buf), &n, nullptr) && n > 0) {
    for (DWORD i = 0; i < n; i++) {
      if (buf[i] != '\n') { if (buf[i] != '\r') line += buf[i]; continue; }
      if (line.rfind("R ", 0) == 0) { int k = atoi(line.c_str() + 2); if (k >= 0 && k < (int)pvPool.slots.size()) pvPool.slots[k].busy.store(0); }
      else if (line.rfind("br ", 0) == 0) { int b = atoi(line.c_str() + 3); if (b >= 100000 && b <= 200000000 && b != gBitrate.load()) { gBitrate.store(b); std::lock_guard<std::mutex> g0(gpuMu); if (pl.hi.enc) { std::lock_guard<std::mutex> g(pl.hi.enc->mu); pl.hi.enc->applyBitrate(b); } } }
      else if (line.rfind("brlo ", 0) == 0) { int b = atoi(line.c_str() + 5); if (b >= 100000 && b <= 50000000 && b != gBitrateLo.load()) { gBitrateLo.store(b); std::lock_guard<std::mutex> g0(gpuMu); if (pl.hasLo && pl.lo.enc) { std::lock_guard<std::mutex> g(pl.lo.enc->mu); pl.lo.enc->applyBitrate(b); } } }
      else if (line == "key") pl.hi.forceKey.store(true);
      else if (line == "keylo") pl.lo.forceKey.store(true);
      else if (line.rfind("pv ", 0) == 0) gPreview.store(atoi(line.c_str() + 3) != 0);
      else if (line == "quit") gQuit.store(true);
      line.clear();
    }
  }
  ExitProcess(0);
}

// ---------------- HDR / 屏幕信息 ----------------
static bool monitorIsHdr(HMONITOR mon) {
  com_ptr<IDXGIFactory1> fac; if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(fac.put())))) return false;
  com_ptr<IDXGIAdapter1> ad;
  for (UINT a = 0; fac->EnumAdapters1(a, ad.put()) == S_OK; a++, ad = nullptr) {
    com_ptr<IDXGIOutput> o;
    for (UINT i = 0; ad->EnumOutputs(i, o.put()) == S_OK; i++, o = nullptr) {
      DXGI_OUTPUT_DESC d; o->GetDesc(&d); if (d.Monitor != mon) continue;
      auto o6 = o.try_as<IDXGIOutput6>(); DXGI_OUTPUT_DESC1 d1; if (o6 && SUCCEEDED(o6->GetDesc1(&d1))) return d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
      return false;
    }
  }
  return false;
}
// 系统设置里"SDR 内容亮度"对应的白点（尼特）。scRGB 的 1.0 是 80 尼特
static float sdrWhiteNits(HMONITOR mon) {
  MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); if (!GetMonitorInfoW(mon, &mi)) return 240;
  UINT32 np = 0, nm = 0; if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 240;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(np); std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return 240;
  for (UINT32 i = 0; i < np; i++) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{}; sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; sn.header.size = sizeof(sn); sn.header.adapterId = paths[i].sourceInfo.adapterId; sn.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, mi.szDevice)) continue;
    DISPLAYCONFIG_SDR_WHITE_LEVEL wl{}; wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL; wl.header.size = sizeof(wl); wl.header.adapterId = paths[i].targetInfo.adapterId; wl.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel) return wl.SDRWhiteLevel / 1000.0f * 80.0f;
  }
  return 240;
}

static void statsLoop(HANDLE parent) {
  long long pa = 0, pp = 0, pe = 0, pb = 0, pd = 0, plat = 0, pn = 0, plf = 0, plb = 0; double pt = nowSec();
  while (!gQuit.load()) {
    for (int i = 0; i < 20 && !gQuit.load(); i++) { Sleep(50); pl.tick(); if (WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) return; }
    Encoder* e = pl.enc; if (!e) continue; // 窗口改大小时管线会重建，这里只读原子计数，读到旧的也只是这一秒的数不准
    double t = nowSec(), dt = t - pt; pt = t;
    long long a = pl.arrived.load(), p = pl.paced.load(), ef = e->encFrames.load(), eb = e->encBytes.load(), d = e->busyDrops.load(), l = e->latSumUs.load(), n = e->latN.load();
    long long lf = 0, lb = 0; { std::lock_guard<std::mutex> g(gpuMu); if (pl.hasLo && pl.lo.enc) { lf = pl.lo.enc->encFrames.load(); lb = pl.lo.enc->encBytes.load(); } }
    event("{\"ev\":\"stats\",\"src\":%.1f,\"cap\":%.1f,\"enc\":%.1f,\"kbps\":%.0f,\"drop\":%lld,\"ms\":%.1f,\"w\":%d,\"h\":%d,\"loenc\":%.1f,\"lokbps\":%.0f}",
      (a - pa) / dt, (p - pp) / dt, (ef - pe) / dt, (eb - pb) * 8 / dt / 1000, d - pd, n > pn ? (l - plat) / 1000.0 / (n - pn) : 0.0, pl.outW, pl.outH,
      lf >= plf ? (lf - plf) / dt : 0.0, lb >= plb ? (lb - plb) * 8 / dt / 1000 : 0.0);
    plf = lf; plb = lb;
    pa = a; pp = p; pe = ef; pb = eb; pd = d; plat = l; pn = n;
  }
}

static void announce(const char* ev) {
  event("{\"ev\":\"%s\",\"w\":%d,\"h\":%d,\"srcw\":%d,\"srch\":%d,\"fps\":%d,\"hdr\":%s,\"white\":%.0f,\"enc\":\"%s\",\"pw\":%d,\"ph\":%d,\"low\":%d,\"loh\":%d,\"lofps\":%d,\"handles\":%s}",
    ev, pl.outW, pl.outH, pl.srcW, pl.srcH, A.fps, pl.hdr ? "true" : "false", pl.hdr ? 80.0f / pl.whiteScale : 0.0f, pl.enc ? pl.enc->name.c_str() : "", pl.pvW, pl.pvH,
    pl.hasLo ? pl.lo.w : 0, pl.hasLo ? pl.lo.h : 0, pl.hasLo ? pl.lo.fps : 0, pvPool.handlesJson().c_str());
}

// ---------------- test：测试画面 ----------------
static int runTest(HANDLE parent) {
  int W = A.maxw ? A.maxw : 2560, H = A.maxh ? A.maxh : 1440; A.maxw = W; A.maxh = H;
  HRESULT hr = pl.init(W, H, false, 1, parent); if (FAILED(hr)) return die("pipeline", hr);
  announce("init");
  D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  com_ptr<ID3D11Texture2D> tex; com_ptr<ID3D11RenderTargetView> rtv; dev->CreateTexture2D(&d, nullptr, tex.put()); dev->CreateRenderTargetView(tex.get(), nullptr, rtv.put());
  auto c1 = ctx.as<ID3D11DeviceContext1>();
  HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { statsLoop((HANDLE)p); return 0; }, parent, 0, nullptr);
  double t0 = nowSec(); long long n = 0; unsigned seed = 1;
  while (!gQuit.load()) {
    double due = t0 + (double)n / A.fps, t = nowSec();
    if (t < due) { double ms = (due - t) * 1000; if (ms > 1.5) Sleep((DWORD)(ms - 1)); continue; }
    n++;
    {
      std::lock_guard<std::mutex> g(gpuMu);
      float bg[4] = { 0.08f, 0.09f, 0.11f, 1 }; ctx->ClearRenderTargetView(rtv.get(), bg);
      int x = (int)((n * 16) % W); D3D11_RECT r = { x, 0, std::min(W, x + 80), H }; float fg[4] = { 0.9f, 0.9f, 0.95f, 1 }; c1->ClearView(rtv.get(), fg, &r, 1);
      for (int i = 0; i < 200; i++) { seed = seed * 1664525u + 1013904223u; int bx = (seed >> 8) % (W - 120), by = (seed >> 20) % (H - 120); D3D11_RECT q = { bx, by, bx + 120, by + 120 }; float c[4] = { ((seed >> 3) & 255) / 255.f, ((seed >> 11) & 255) / 255.f, ((seed >> 19) & 255) / 255.f, 1 }; c1->ClearView(rtv.get(), c, &q, 1); }
    }
    pl.onFrame(tex.get());
  }
  WaitForSingleObject(th, 500);
  return 0;
}

// ---------------- 采集（Windows Graphics Capture） ----------------
static int runCapture(HANDLE parent) {
  HMONITOR mon = nullptr; HWND hwnd = nullptr;
  if (A.mode == "monitor") { POINT pt{ (LONG)A.t1, (LONG)A.t2 }; mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY); }
  else { hwnd = (HWND)(UINT_PTR)A.t1; if (!IsWindow(hwnd)) return die("no-window"); mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST); }
  if (!wgc::GraphicsCaptureSession::IsSupported()) return die("wgc-unsupported");
  bool hdr = monitorIsHdr(mon); float white = hdr ? sdrWhiteNits(mon) : 80;

  com_ptr<::IInspectable> insp; HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dev.as<IDXGIDevice>().get(), insp.put()); if (FAILED(hr)) return die("winrt-device", hr);
  auto wdev = insp.as<wdx::Direct3D11::IDirect3DDevice>();
  auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  wgc::GraphicsCaptureItem item{ nullptr };
  hr = hwnd ? interop->CreateForWindow(hwnd, winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item)) : interop->CreateForMonitor(mon, winrt::guid_of<wgc::IGraphicsCaptureItem>(), winrt::put_abi(item));
  if (FAILED(hr) || !item) return die("capture-item", hr);
  auto size = item.Size();
  auto fmt = hdr ? wdx::DirectXPixelFormat::R16G16B16A16Float : wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
  if (FAILED(hr = pl.init(size.Width, size.Height, hdr, 80.0f / white, parent))) return die("pipeline", hr);
  announce("init");

  auto framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(wdev, fmt, 2, size);
  auto session = framePool.CreateCaptureSession(item);
  try { session.IsCursorCaptureEnabled(A.cursor != 0); } catch (...) {}
  try { session.IsBorderRequired(false); } catch (...) {}                       // Windows 11：不画黄色边框
  try { session.MinUpdateInterval(std::chrono::milliseconds(1)); } catch (...) {} // 新系统上默认间隔偏大，放开到显示器刷新率
  std::atomic<bool> closed{false}; std::mutex frameMu; winrt::Windows::Graphics::SizeInt32 cur = size;
  framePool.FrameArrived([&](wgc::Direct3D11CaptureFramePool const& fp, winrt::Windows::Foundation::IInspectable const&) {
    std::lock_guard<std::mutex> g(frameMu);
    auto frame = fp.TryGetNextFrame(); if (!frame) return;
    auto cs = frame.ContentSize();
    if (cs.Width != cur.Width || cs.Height != cur.Height) { // 窗口大小变了：重建帧池和编码器
      if (cs.Width < 16 || cs.Height < 16) return;
      cur = cs; frame.Close();
      fp.Recreate(wdev, fmt, 2, cs);
      HRESULT h2; { std::lock_guard<std::mutex> g2(gpuMu); pl.destroy(); h2 = pl.init(cs.Width, cs.Height, hdr, 80.0f / white, parent); }
      if (FAILED(h2)) { die("pipeline-resize", h2); gQuit.store(true); return; }
      announce("size");
      return;
    }
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> tex; if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) pl.onFrame(tex.get());
  });
  item.Closed([&](auto const&, auto const&) { closed.store(true); });
  session.StartCapture();
  HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { statsLoop((HANDLE)p); return 0; }, parent, 0, nullptr);
  while (!gQuit.load() && !closed.load()) { if (WaitForSingleObject(parent, 100) == WAIT_OBJECT_0) break; }
  if (closed.load()) event("{\"ev\":\"closed\"}");
  gQuit.store(true);
  try { session.Close(); framePool.Close(); } catch (...) {}
  WaitForSingleObject(th, 500);
  return 0;
}

static bool parse(int argc, char** argv) {
  if (argc < 3) return false;
  A.parent = strtoul(argv[1], nullptr, 10); A.mode = argv[2]; int i = 3;
  if (A.mode == "monitor") { if (argc < 5) return false; A.t1 = _atoi64(argv[3]); A.t2 = _atoi64(argv[4]); i = 5; }
  else if (A.mode == "window") { if (argc < 4) return false; A.t1 = _atoi64(argv[3]); i = 4; }
  else if (A.mode != "test") return false;
  for (; i + 1 < argc; i += 2) {
    std::string k = argv[i]; int v = atoi(argv[i + 1]);
    if (k == "--fps") A.fps = v; else if (k == "--maxw") A.maxw = v; else if (k == "--maxh") A.maxh = v; else if (k == "--bitrate") A.bitrate = v;
    else if (k == "--cursor") A.cursor = v; else if (k == "--preview") A.preview = v; else if (k == "--pvw") A.pvw = v;
    else if (k == "--lobitrate") A.lobitrate = v; else if (k == "--lomaxh") A.lomaxh = v; else if (k == "--lofps") A.lofps = v;
  }
  if (A.fps < 1 || A.fps > 480) A.fps = 60;
  if (A.bitrate < 100000) A.bitrate = 8000000;
  if (A.pvw < 160 || A.pvw > 1920) A.pvw = 960;
  if (A.lomaxh < 180 || A.lomaxh > 2160) A.lomaxh = 720;
  if (A.lofps < 5 || A.lofps > 120) A.lofps = 30;
  gBitrate.store(A.bitrate); gBitrateLo.store(A.lobitrate); gPreview.store(A.preview != 0);
  return A.parent != 0;
}

int main(int argc, char** argv) {
  _setmode(_fileno(stdout), _O_BINARY);
  if (!parse(argc, argv)) { fprintf(stderr, "usage: lc-capture <parentPid> monitor <x> <y> | window <hwnd> | test  [--fps N --maxw W --maxh H --bitrate BPS --cursor 0|1 --preview 0|1 --pvw W]\n"); return 1; }
  HANDLE parent = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, A.parent);
  if (!parent) return die("open-parent", HRESULT_FROM_WIN32(GetLastError()));
  CreateThread(nullptr, 0, stdinThread, nullptr, 0, nullptr);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL); if (FAILED(hr)) return die("mfstartup", hr);
  // 显卡调度优先级提到"高"：游戏把显卡占满时，我们这点工作（两遍着色器）不排在游戏后面等
  { typedef LONG (WINAPI *SetFn)(HANDLE, int); HMODULE gdi = LoadLibraryW(L"gdi32.dll"); SetFn set = gdi ? (SetFn)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass") : nullptr; if (set) set(GetCurrentProcess(), 4); }
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, fl, 2, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
  if (FAILED(hr)) return die("device", hr);
  if (auto mt = dev.try_as<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);
  { auto dx = dev.as<IDXGIDevice>(); com_ptr<IDXGIAdapter> ad; dx->GetAdapter(ad.put()); DXGI_ADAPTER_DESC d; ad->GetDesc(&d); adapterLuid = d.AdapterLuid; dx->SetGPUThreadPriority(7); }
  if (FAILED(hr = sh.init())) return die("shaders", hr);
  int rc = A.mode == "test" ? runTest(parent) : runCapture(parent);
  { std::lock_guard<std::mutex> g(gpuMu); pl.destroy(); }
  MFShutdown();
  return rc;
}
