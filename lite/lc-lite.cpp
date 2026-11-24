// LiveCord 轻量版 —— WebView2 外壳
//
// 和完整版（Electron）做同一件事：开一个窗口加载线上网站，给页面一个 window.lcDesktop 接口，
// 画面和声音由 bin\lc-capture.exe / bin\lc-audio.exe 两个助手完成。区别是不自带浏览器内核，用系统里的 WebView2（Edge 内核），
// 所以安装包只有几 MB。代价：内核版本由系统自动更新，不归我们管 —— 高帧率直播靠的那几条规则（见 public/desktop-vworker.js）
// 是在固定版本上实测出来的，系统更新后可能失效。
//
// 页面那一半在 shim.js（注入到每个页面里），两边的约定：
//   页面 → 外壳：一段文字 "编号␟方法␟参数…"（␟ = U+001F）
//   外壳 → 页面：JSON。{"id":编号,"r":结果} / {"id":编号,"e":"错误"}；事件 {"ev":"win|audio|video","g":第几次启动,"d":{…}}；
//                数据通知 {"b":[[类型,第几次启动,位置,长度,时间戳,标志],…]}，数据本身在共享内存里（类型 1 码流 2 声音 4 预览）
//
// 构建：lite\build.cmd（要 Visual Studio 的 C++ 工具和 webview2sdk 目录里的开发包）
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <psapi.h>
#include <wincrypt.h>
#include <dxgi1_6.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")
using Microsoft::WRL::ComPtr; using Microsoft::WRL::Callback; using Microsoft::WRL::Make;

#define LC_VERSION L"1.0.2"
static const wchar_t* PROD_URL = L"https://live.felisnyan.com";
enum { WM_APP_NOTES = WM_APP + 1, WM_APP_JSON, WM_APP_REPLY, WM_APP_RELOAD };
enum { TIMER_RELOAD = 1 };

// ---------------- 全局 ----------------
static HINSTANCE gInst; static HWND gWnd;
static ComPtr<ICoreWebView2Environment> gEnv; static ComPtr<ICoreWebView2Controller> gCtl; static ComPtr<ICoreWebView2> gWeb;
static std::wstring gUrl, gOrigin, gExeDir, gDataDir, gBrowserVer, gDebugPort;
static bool gHidden = false, gNoActivate = false, gLive = false, gFull = false, gOffline = false, gHwAccel = true;
static HANDLE gJob = nullptr, gMutex = nullptr;
static struct { int x = CW_USEDEFAULT, y = 0, w = 1280, h = 800; bool max = false, have = false; } gCfg;

// ---------------- 小工具 ----------------
static std::wstring widen(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w; }
static std::string narrow(const std::wstring& w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s; }
static std::wstring jstr(const std::wstring& s) {
  std::wstring o = L"\"";
  for (wchar_t c : s) { if (c == L'"' || c == L'\\') { o += L'\\'; o += c; } else if (c < 0x20) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", c); o += b; } else o += c; }
  return o + L"\"";
}
static std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) { std::vector<std::wstring> v; size_t a = 0; for (;;) { size_t b = s.find(sep, a); if (b == std::wstring::npos) { v.push_back(s.substr(a)); break; } v.push_back(s.substr(a, b - a)); a = b + 1; } return v; }
static bool digits(const std::wstring& s) { if (s.empty() || s.size() > 12) return false; for (wchar_t c : s) if (c < L'0' || c > L'9') return false; return true; }
static bool startsWith(const std::wstring& s, const wchar_t* p) { size_t n = wcslen(p); return s.size() >= n && s.compare(0, n, p) == 0; }
// 这个地址是不是本站的页面
static bool fromApp(const std::wstring& uri) {
  if (uri.size() < gOrigin.size() || _wcsnicmp(uri.c_str(), gOrigin.c_str(), gOrigin.size()) != 0) return false;
  wchar_t c = uri.size() > gOrigin.size() ? uri[gOrigin.size()] : L'/';
  return c == L'/' || c == L'?' || c == L'#';
}
static std::wstring envVar(const wchar_t* k) { wchar_t b[1024]; DWORD n = GetEnvironmentVariableW(k, b, 1024); return n && n < 1024 ? std::wstring(b, n) : L""; }
static bool fileExists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static std::wstring helperPath(const wchar_t* name) {
  for (const wchar_t* rel : { L"\\bin\\", L"\\..\\bin\\", L"\\..\\..\\bin\\" }) { std::wstring p = gExeDir + rel + name; if (fileExists(p)) return p; }
  return gExeDir + L"\\bin\\" + name;
}
static std::string resourceText(int id) {
  HRSRC r = FindResourceW(gInst, MAKEINTRESOURCEW(id), RT_RCDATA); if (!r) return "";
  HGLOBAL g = LoadResource(gInst, r); if (!g) return "";
  return std::string((const char*)LockResource(g), SizeofResource(gInst, r));
}
static void replaceAll(std::wstring& s, const std::wstring& a, const std::wstring& b) { for (size_t p = 0; (p = s.find(a, p)) != std::wstring::npos; p += b.size()) s.replace(p, a.size(), b); }

// ---------------- 设置 ----------------
static std::wstring cfgFile() { return gDataDir + L"\\lite.cfg"; }
static void loadCfg() {
  FILE* f = nullptr; if (_wfopen_s(&f, cfgFile().c_str(), L"r") || !f) return;
  char line[128]; std::map<std::string, int> m;
  while (fgets(line, sizeof(line), f)) { char* eq = strchr(line, '='); if (eq) { *eq = 0; m[line] = atoi(eq + 1); } }
  fclose(f);
  if (m.count("w") && m["w"] >= 400 && m["h"] >= 300) { gCfg.x = m["x"]; gCfg.y = m["y"]; gCfg.w = m["w"]; gCfg.h = m["h"]; gCfg.have = true; }
  gCfg.max = m["max"] != 0; if (m.count("hwAccel")) gHwAccel = m["hwAccel"] != 0;
}
static void saveCfg() {
  if (gHidden) return;
  FILE* f = nullptr; if (_wfopen_s(&f, cfgFile().c_str(), L"w") || !f) return;
  fprintf(f, "x=%d\ny=%d\nw=%d\nh=%d\nmax=%d\nhwAccel=%d\n", gCfg.x, gCfg.y, gCfg.w, gCfg.h, gCfg.max ? 1 : 0, gHwAccel ? 1 : 0); fclose(f);
}
static void rememberBounds() {
  if (!gWnd || gFull || gHidden) return;
  WINDOWPLACEMENT pl{ sizeof(pl) }; if (!GetWindowPlacement(gWnd, &pl) || pl.showCmd == SW_SHOWMINIMIZED) return;
  gCfg.max = pl.showCmd == SW_SHOWMAXIMIZED;
  RECT r = pl.rcNormalPosition; gCfg.x = r.left; gCfg.y = r.top; gCfg.w = r.right - r.left; gCfg.h = r.bottom - r.top; gCfg.have = true;
}

// ---------------- 发给页面 ----------------
static void postJson(const std::wstring& j) { if (gWeb) gWeb->PostWebMessageAsJson(j.c_str()); }
static void postJsonAsync(std::wstring j) { PostMessageW(gWnd, WM_APP_JSON, 0, (LPARAM) new std::wstring(std::move(j))); } // 别的线程用
struct Reply { int id; bool err; std::wstring body; };
static void replyNow(int id, const std::wstring& json) { postJson(L"{\"id\":" + std::to_wstring(id) + L",\"r\":" + json + L"}"); }
static void failNow(int id, const std::wstring& msg) { postJson(L"{\"id\":" + std::to_wstring(id) + L",\"e\":" + jstr(msg) + L"}"); }
static void replyAsync(int id, std::wstring json, bool err = false) { PostMessageW(gWnd, WM_APP_REPLY, 0, (LPARAM) new Reply{ id, err, std::move(json) }); }

// ---------------- 共享内存（外壳写，页面只读） ----------------
// 开头 16 字节是头（前 8 字节 = 到现在一共写了多少字节），后面是环形区。每一段数据连续存放，尾巴放不下就从头开始
static const UINT64 RING_HDR = 16, RING_CAP = 32ull << 20, RING_MAX_ITEM = 4ull << 20;
static ComPtr<ICoreWebView2SharedBuffer> gShared; static BYTE* gRing = nullptr; static UINT64 gHead = 0; static std::mutex gRingMu;
struct Note { int type, gen; UINT64 pos; DWORD len; UINT64 ts; int flags; };
static std::mutex gNoteMu; static std::vector<Note> gNotes; static std::atomic<bool> gNotePosted{ false };
static void ringWrite(int type, int gen, const BYTE* d, DWORD len, UINT64 ts, int flags) {
  if (!gRing || !len || len > RING_MAX_ITEM) return;
  Note n{ type, gen, 0, len, ts, flags };
  {
    std::lock_guard<std::mutex> g(gRingMu);
    UINT64 off = gHead % RING_CAP; if (off + len > RING_CAP) { gHead += RING_CAP - off; off = 0; }
    memcpy(gRing + RING_HDR + off, d, len); n.pos = gHead; gHead += len; memcpy(gRing, &gHead, 8);
  }
  { std::lock_guard<std::mutex> g(gNoteMu); gNotes.push_back(n); }
  if (!gNotePosted.exchange(true)) PostMessageW(gWnd, WM_APP_NOTES, 0, 0);
}
static void flushNotes() {
  gNotePosted.store(false);
  std::vector<Note> v; { std::lock_guard<std::mutex> g(gNoteMu); v.swap(gNotes); }
  if (v.empty() || !gWeb) return;
  std::wstring j = L"{\"b\":["; wchar_t b[128];
  for (size_t i = 0; i < v.size(); i++) { swprintf_s(b, L"%s[%d,%d,%llu,%lu,%llu,%d]", i ? L"," : L"", v[i].type, v[i].gen, v[i].pos, v[i].len, v[i].ts, v[i].flags); j += b; }
  postJson(j + L"]}");
}

// ---------------- 助手进程 ----------------
static std::mutex gSpawnMu; // 建管道 + 起进程要排队：不然同时起的两个进程会互相继承对方的管道
struct Proc { HANDLE h = nullptr, in = nullptr, out = nullptr, err = nullptr; DWORD pid = 0;
  ~Proc() { for (HANDLE x : { in, out, err, h }) if (x) CloseHandle(x); } };
static std::shared_ptr<Proc> spawnProc(const std::wstring& cmd, bool wantIn, bool wantErr, DWORD outBuf) {
  std::lock_guard<std::mutex> g(gSpawnMu);
  SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
  HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  if (wantIn) { CreatePipe(&inR, &inW, &sa, 0); SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0); }
  CreatePipe(&outR, &outW, &sa, outBuf); SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  if (wantErr) { CreatePipe(&errR, &errW, &sa, 0); SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0); }
  STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = wantIn ? inR : nul; si.hStdOutput = outW; si.hStdError = wantErr ? errW : nul;
  PROCESS_INFORMATION pi{}; std::wstring c = cmd;
  BOOL ok = CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  for (HANDLE x : { inR, outW, errW, nul }) if (x && x != INVALID_HANDLE_VALUE) CloseHandle(x);
  if (!ok) { for (HANDLE x : { inW, outR, errR }) if (x) CloseHandle(x); return nullptr; }
  if (gJob) AssignProcessToJobObject(gJob, pi.hProcess); // 外壳没了助手也跟着退
  CloseHandle(pi.hThread);
  auto p = std::make_shared<Proc>(); p->h = pi.hProcess; p->pid = pi.dwProcessId; p->in = inW; p->out = outR; p->err = errR;
  return p;
}
static void writeLine(const std::shared_ptr<Proc>& p, const std::string& line) { if (p && p->in) { DWORD wr; std::string s = line + "\n"; WriteFile(p->in, s.data(), (DWORD)s.size(), &wr, nullptr); } }
// 给它一点时间自己退，不退就杀
static void retire(std::shared_ptr<Proc> p, DWORD graceMs) { if (!p) return; std::thread([p, graceMs] { if (WaitForSingleObject(p->h, graceMs) == WAIT_TIMEOUT) TerminateProcess(p->h, 1); }).detach(); }
static DWORD exitCodeOf(const std::shared_ptr<Proc>& p) { WaitForSingleObject(p->h, 3000); DWORD c = 0; GetExitCodeProcess(p->h, &c); return c; }

// ---- 声音：lc-audio capture，标准输出是 48k 双声道 float32 ----
static std::shared_ptr<Proc> gAudio; static int gAudioGen = 0;
static void stopAudio() {
  auto p = gAudio; gAudio = nullptr; gAudioGen++;
  if (!p) return;
  if (p->in) { CloseHandle(p->in); p->in = nullptr; }
  retire(p, 500);
}
static bool startAudio(const std::wstring& mode, const std::wstring& pid) {
  stopAudio();
  std::wstring target = mode == L"all" ? std::to_wstring(GetCurrentProcessId()) : pid; // 全部声音：排除自己这棵进程树，别人直播的声音和音效不会被播回去
  std::wstring cmd = L"\"" + helperPath(L"lc-audio.exe") + L"\" capture " + target + (mode == L"app" ? L" include" : L" exclude");
  auto p = spawnProc(cmd, true, true, 1 << 16); if (!p) return false;
  gAudio = p; int gen = gAudioGen;
  auto errText = std::make_shared<std::string>(); auto errMu = std::make_shared<std::mutex>();
  std::thread([p, gen, errText, errMu] {
    char b[512]; DWORD n; std::string pend;
    while (ReadFile(p->err, b, sizeof(b), &n, nullptr) && n) {
      pend.append(b, n);
      size_t r = pend.find("ready");
      if (r != std::string::npos) { postJsonAsync(L"{\"ev\":\"audio\",\"g\":" + std::to_wstring(gen) + L",\"d\":{\"state\":\"ready\"}}"); pend.erase(0, r + 5); }
      std::lock_guard<std::mutex> g(*errMu); *errText = pend;
    }
  }).detach();
  std::thread([p, gen, errText, errMu] {
    std::vector<BYTE> buf(1 << 16); DWORD n; size_t have = 0;
    while (ReadFile(p->out, buf.data() + have, (DWORD)(buf.size() - have), &n, nullptr) && n) {
      have += n; size_t whole = have - (have % 8); // 管道给的数据不保证按整帧（8 字节）切
      if (whole) { ringWrite(2, gen, buf.data(), (DWORD)whole, 0, 0); memmove(buf.data(), buf.data() + whole, have - whole); have -= whole; }
    }
    DWORD code = exitCodeOf(p); Sleep(50);
    std::string detail; { std::lock_guard<std::mutex> g(*errMu); detail = *errText; }
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r' || detail.back() == ' ')) detail.pop_back();
    postJsonAsync(L"{\"ev\":\"audio\",\"g\":" + std::to_wstring(gen) + L",\"d\":{\"state\":\"" + (code == 3 ? L"target-exit" : L"error") + L"\",\"detail\":" + jstr(widen(detail)) + L"}}");
  }).detach();
  return true;
}

// ---- 画面：lc-capture，标准输出是 16 字节头 + 数据（类型 1 码流，2 事件 JSON，4 预览画面） ----
static std::shared_ptr<Proc> gVideo; static int gVideoGen = 0;
static void stopVideo() {
  auto p = gVideo; gVideo = nullptr; gVideoGen++;
  if (!p) return;
  writeLine(p, "quit"); retire(p, 1500);
}
struct Mon { HMONITOR h; RECT rc; bool primary; int hz; bool hdr; };
static std::vector<Mon> gMons; static std::mutex gMonMu;
static std::vector<Mon> enumMons() {
  std::vector<Mon> v;
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); if (!GetMonitorInfoW(h, &mi)) return TRUE;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm); int hz = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
    ((std::vector<Mon>*)lp)->push_back({ h, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, hz, false });
    return TRUE;
  }, (LPARAM)&v);
  ComPtr<IDXGIFactory1> fac;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
    ComPtr<IDXGIAdapter1> ad;
    for (UINT a = 0; fac->EnumAdapters1(a, &ad) == S_OK; a++) {
      ComPtr<IDXGIOutput> o;
      for (UINT i = 0; ad->EnumOutputs(i, &o) == S_OK; i++) {
        ComPtr<IDXGIOutput6> o6; DXGI_OUTPUT_DESC1 d{};
        if (SUCCEEDED(o.As(&o6)) && SUCCEEDED(o6->GetDesc1(&d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) for (auto& m : v) if (m.h == d.Monitor) m.hdr = true;
        o.Reset();
      }
      ad.Reset();
    }
  }
  std::stable_sort(v.begin(), v.end(), [](const Mon& a, const Mon& b) { return a.primary != b.primary ? a.primary : a.rc.left < b.rc.left; });
  return v;
}
// kv: source=…、fps=…、bitrate=… 这些（页面给的，只认名单里的名字和纯数字的值）
static bool startVideo(const std::vector<std::wstring>& kv, std::wstring& why) {
  stopVideo();
  std::wstring cmd = L"\"" + helperPath(L"lc-capture.exe") + L"\" " + std::to_wstring(GetCurrentProcessId()), src, opts;
  static const wchar_t* KEYS[] = { L"fps", L"maxw", L"maxh", L"bitrate", L"cursor", L"preview", L"lobitrate", L"lomaxh", L"lofps", L"codec" };
  for (auto& s : kv) {
    size_t eq = s.find(L'='); if (eq == std::wstring::npos) continue;
    std::wstring k = s.substr(0, eq), v = s.substr(eq + 1);
    if (k == L"source") { src = v; continue; }
    if (!digits(v)) continue;
    for (auto K : KEYS) if (k == K) opts += L" --" + k + L" " + v;
  }
  auto parts = split(src, L':');
  if (src == L"test") cmd += L" test";
  else if (parts.size() >= 2 && parts[0] == L"window" && digits(parts[1])) cmd += L" window " + parts[1];
  else if (parts.size() >= 2 && parts[0] == L"screen" && digits(parts[1])) {
    // 屏幕：把这块屏幕中心点的坐标给助手，它自己找对应的显示器
    std::vector<Mon> ms; { std::lock_guard<std::mutex> g(gMonMu); ms = gMons; }
    if (ms.empty()) { ms = enumMons(); std::lock_guard<std::mutex> g(gMonMu); gMons = ms; }
    size_t i = (size_t)_wtoi(parts[1].c_str()); if (ms.empty()) { why = L"没有找到屏幕"; return false; } if (i >= ms.size()) i = 0;
    cmd += L" monitor " + std::to_wstring((ms[i].rc.left + ms[i].rc.right) / 2) + L" " + std::to_wstring((ms[i].rc.top + ms[i].rc.bottom) / 2);
  } else { why = L"bad source"; return false; }
  cmd += opts + L" --pvw 640 --pvraw 1";
  auto p = spawnProc(cmd, true, false, 1 << 20); if (!p) { why = L"采集助手启动失败"; return false; }
  gVideo = p; int gen = gVideoGen;
  std::thread([p, gen] {
    std::vector<BYTE> buf(1 << 20); size_t have = 0; DWORD n; const std::wstring pre = L"{\"ev\":\"video\",\"g\":" + std::to_wstring(gen) + L",\"d\":";
    for (;;) {
      if (have == buf.size()) buf.resize(buf.size() * 2);
      if (!ReadFile(p->out, buf.data() + have, (DWORD)(buf.size() - have), &n, nullptr) || !n) break;
      have += n; size_t at = 0;
      while (have - at >= 16) {
        DWORD len; memcpy(&len, buf.data() + at, 4); if (len > (64u << 20)) { have = at = 0; break; }
        if (have - at < 16 + (size_t)len) { if (buf.size() < 16 + (size_t)len) buf.resize(16 + (size_t)len + 4096); break; }
        BYTE type = buf[at + 4], flags = buf[at + 5]; UINT64 ts; memcpy(&ts, buf.data() + at + 8, 8); const BYTE* d = buf.data() + at + 16;
        if (type == 1) ringWrite(1, gen, d, len, ts, flags);
        else if (type == 4) ringWrite(4, gen, d, len, ts, 0);
        else if (type == 2) postJsonAsync(pre + widen(std::string((const char*)d, len)) + L"}");
        at += 16 + (size_t)len;
      }
      if (at) { memmove(buf.data(), buf.data() + at, have - at); have -= at; }
    }
    postJsonAsync(pre + L"{\"ev\":\"exit\",\"code\":" + std::to_wstring((int)exitCodeOf(p)) + L"}}");
  }).detach();
  return true;
}
static bool videoCmd(const std::wstring& line) {
  bool ok = line == L"key" || line == L"keylo";
  for (const wchar_t* pre : { L"br ", L"brlo ", L"pv " }) if (startsWith(line, pre) && digits(line.substr(wcslen(pre)))) ok = true;
  if (ok && gVideo) writeLine(gVideo, narrow(line));
  return ok;
}

// ---------------- 图片：屏幕缩略图、程序图标 ----------------
static bool encoderClsid(const wchar_t* mime, CLSID& id) {
  UINT n = 0, sz = 0; Gdiplus::GetImageEncodersSize(&n, &sz); if (!sz) return false;
  std::vector<BYTE> b(sz); auto* e = (Gdiplus::ImageCodecInfo*)b.data(); Gdiplus::GetImageEncoders(n, sz, e);
  for (UINT i = 0; i < n; i++) if (!wcscmp(e[i].MimeType, mime)) { id = e[i].Clsid; return true; }
  return false;
}
static std::wstring dataUrl(Gdiplus::Bitmap& bmp, const wchar_t* mime) {
  CLSID id; if (!encoderClsid(mime, id)) return L"";
  ComPtr<IStream> st; if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) return L"";
  if (bmp.Save(st.Get(), &id, nullptr) != Gdiplus::Ok) return L"";
  HGLOBAL hg = nullptr; GetHGlobalFromStream(st.Get(), &hg); STATSTG sg{}; st->Stat(&sg, STATFLAG_NONAME);
  DWORD len = (DWORD)sg.cbSize.QuadPart, out = 0; const BYTE* p = (const BYTE*)GlobalLock(hg);
  CryptBinaryToStringW(p, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &out);
  std::wstring b64(out, 0); CryptBinaryToStringW(p, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &out); b64.resize(out);
  GlobalUnlock(hg);
  return std::wstring(L"data:") + mime + L";base64," + b64;
}
static std::wstring screensJson(int tw) {
  auto ms = enumMons(); { std::lock_guard<std::mutex> g(gMonMu); gMons = ms; }
  std::wstring j = L"["; HDC scr = GetDC(nullptr);
  for (size_t i = 0; i < ms.size(); i++) {
    int w = ms[i].rc.right - ms[i].rc.left, h = ms[i].rc.bottom - ms[i].rc.top; std::wstring thumb;
    if (tw > 0 && w > 0 && h > 0) {
      int th = std::max(1, tw * h / w); HDC mem = CreateCompatibleDC(scr); HBITMAP bm = CreateCompatibleBitmap(scr, tw, th); HGDIOBJ old = SelectObject(mem, bm);
      SetStretchBltMode(mem, HALFTONE); SetBrushOrgEx(mem, 0, 0, nullptr);
      StretchBlt(mem, 0, 0, tw, th, scr, ms[i].rc.left, ms[i].rc.top, w, h, SRCCOPY | CAPTUREBLT);
      SelectObject(mem, old);
      { Gdiplus::Bitmap bmp(bm, nullptr); thumb = dataUrl(bmp, L"image/jpeg"); }
      DeleteObject(bm); DeleteDC(mem);
    }
    wchar_t b[200]; swprintf_s(b, L"%s{\"idx\":%d,\"w\":%d,\"h\":%d,\"hz\":%d,\"primary\":%s,\"hdr\":%s,\"thumb\":", i ? L"," : L"", (int)i, w, h, ms[i].hz, ms[i].primary ? L"true" : L"false", ms[i].hdr ? L"true" : L"false");
    j += b; j += thumb.empty() ? L"null" : jstr(thumb); j += L"}";
  }
  ReleaseDC(nullptr, scr);
  return j + L"]";
}
static std::wstring iconJson(const std::wstring& path) {
  SHFILEINFOW fi{}; if (!SHGetFileInfoW(path.c_str(), 0, &fi, sizeof(fi), SHGFI_ICON | SHGFI_LARGEICON) || !fi.hIcon) return L"null";
  const int S = 32; BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr; HDC dc = CreateCompatibleDC(nullptr); HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0); std::wstring url;
  if (bm && bits) {
    HGDIOBJ old = SelectObject(dc, bm); memset(bits, 0, S * S * 4); DrawIconEx(dc, 0, 0, fi.hIcon, S, S, 0, nullptr, DI_NORMAL); GdiFlush(); SelectObject(dc, old);
    BYTE* p = (BYTE*)bits; bool anyAlpha = false; for (int i = 0; i < S * S; i++) if (p[i * 4 + 3]) { anyAlpha = true; break; }
    if (!anyAlpha) for (int i = 0; i < S * S; i++) p[i * 4 + 3] = 255; // 老式图标没有透明通道
    { Gdiplus::Bitmap bmp(S, S, S * 4, PixelFormat32bppARGB, p); url = dataUrl(bmp, L"image/png"); }
  }
  if (bm) DeleteObject(bm); DeleteDC(dc); DestroyIcon(fi.hIcon);
  return url.empty() ? L"null" : jstr(url);
}
static std::string runCapture(const std::wstring& cmd, DWORD timeoutMs) {
  auto p = spawnProc(cmd, false, false, 1 << 16); if (!p) return "";
  std::thread([p, timeoutMs] { if (WaitForSingleObject(p->h, timeoutMs) == WAIT_TIMEOUT) TerminateProcess(p->h, 1); }).detach();
  std::string out; char b[8192]; DWORD n; while (ReadFile(p->out, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
  return out;
}

// ---------------- 窗口状态、占用 ----------------
static std::wstring winStateJson() {
  bool vis = gWnd && IsWindowVisible(gWnd), mini = gWnd && IsIconic(gWnd);
  wchar_t b[200]; swprintf_s(b, L"{\"focused\":%s,\"minimized\":%s,\"visible\":%s,\"maximized\":%s,\"fullscreen\":%s}",
    (!gHidden && GetForegroundWindow() == gWnd) ? L"true" : L"false", mini ? L"true" : L"false", vis ? L"true" : L"false", (gWnd && IsZoomed(gWnd)) ? L"true" : L"false", gFull ? L"true" : L"false");
  return b;
}
static void sendWinState() { postJson(L"{\"ev\":\"win\",\"d\":" + winStateJson() + L"}"); }
static std::vector<std::pair<DWORD, const wchar_t*>> ownProcs() {
  std::vector<std::pair<DWORD, const wchar_t*>> v; v.push_back({ GetCurrentProcessId(), L"Browser" });
  ComPtr<ICoreWebView2Environment8> e8; ComPtr<ICoreWebView2ProcessInfoCollection> col;
  if (gEnv && SUCCEEDED(gEnv.As(&e8)) && SUCCEEDED(e8->GetProcessInfos(&col)) && col) {
    UINT n = 0; col->get_Count(&n);
    for (UINT i = 0; i < n; i++) {
      ComPtr<ICoreWebView2ProcessInfo> pi; if (FAILED(col->GetValueAtIndex(i, &pi))) continue;
      INT32 pid = 0; COREWEBVIEW2_PROCESS_KIND k{}; pi->get_ProcessId(&pid); pi->get_Kind(&k);
      v.push_back({ (DWORD)pid, k == COREWEBVIEW2_PROCESS_KIND_BROWSER ? L"Browser" : k == COREWEBVIEW2_PROCESS_KIND_RENDERER ? L"Tab" : k == COREWEBVIEW2_PROCESS_KIND_GPU ? L"GPU" : L"Utility" });
    }
  }
  if (gVideo) v.push_back({ gVideo->pid, L"Utility" });
  if (gAudio) v.push_back({ gAudio->pid, L"Utility" });
  return v;
}
static std::wstring metricsJson() {
  static std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> last; // pid -> (CPU 时间, 当时的时刻)，单位 100 纳秒
  std::wstring j = L"["; bool first = true; FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt); ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
  for (auto& [pid, type] : ownProcs()) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) continue;
    FILETIME c, e, k, u; PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof(pm); double cpu = 0;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
      ULONGLONG t = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
      auto it = last.find(pid); if (it != last.end() && now > it->second.second && t >= it->second.first) cpu = (double)(t - it->second.first) / (double)(now - it->second.second) * 100.0;
      last[pid] = { t, now };
    }
    GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm)); CloseHandle(h);
    wchar_t b[200]; swprintf_s(b, L"%s{\"pid\":%lu,\"type\":\"%s\",\"name\":\"\",\"cpu\":%.2f,\"mem\":%llu,\"priv\":%llu}", first ? L"" : L",", pid, type, cpu, (ULONGLONG)pm.WorkingSetSize / 1024, (ULONGLONG)pm.PrivateUsage / 1024);
    j += b; first = false;
  }
  return j + L"]";
}
static void setLive(bool on) { gLive = on; SetThreadExecutionState(ES_CONTINUOUS | (on ? ES_SYSTEM_REQUIRED : 0)); } // 直播中不让系统休眠
static void setFullscreen(bool on) {
  static WINDOWPLACEMENT prev{ sizeof(prev) }; static LONG_PTR prevStyle = 0;
  if (on == gFull || !gWnd) return;
  if (on) {
    rememberBounds(); prevStyle = GetWindowLongPtrW(gWnd, GWL_STYLE); GetWindowPlacement(gWnd, &prev);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(gWnd, MONITOR_DEFAULTTONEAREST), &mi);
    gFull = true; SetWindowLongPtrW(gWnd, GWL_STYLE, prevStyle & ~(WS_CAPTION | WS_THICKFRAME));
    SetWindowPos(gWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    gFull = false; SetWindowLongPtrW(gWnd, GWL_STYLE, prevStyle); SetWindowPlacement(gWnd, &prev);
    SetWindowPos(gWnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
  }
  sendWinState();
}
static void relaunch() {
  wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = GetCommandLineW(); if (cmd.find(L"--lc-wait=") == std::wstring::npos) cmd += L" --lc-wait=" + std::to_wstring(GetCurrentProcessId());
  saveCfg(); if (gMutex) { CloseHandle(gMutex); gMutex = nullptr; }
  STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
  if (CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  DestroyWindow(gWnd);
}
static void showOffline(const std::wstring& why) {
  std::wstring html = widen(resourceText(102)); gOffline = true;
  replaceAll(html, L"</body>", L"<script>document.getElementById('why').textContent=" + jstr(why) + L";</script></body>");
  if (gWeb) gWeb->NavigateToString(html.c_str());
}

// ---------------- 页面发来的请求 ----------------
static void onMessage(const std::wstring& source, const std::wstring& msg) {
  auto a = split(msg, L'\x1f'); if (a.size() < 2) return;
  int id = _wtoi(a[0].c_str()); const std::wstring& m = a[1];
  if (m == L"retry") { if (!startsWith(source, L"http") && gWeb) { gOffline = false; gWeb->Navigate(gUrl.c_str()); } return; } // 断网页唯一的入口
  if (!fromApp(source)) return failNow(id, L"forbidden");
  auto arg = [&](size_t i) -> std::wstring { return a.size() > i + 2 ? a[i + 2] : L""; };
  if (m == L"hello") {
    ComPtr<ICoreWebView2_17> w17;
    if (gShared && SUCCEEDED(gWeb.As(&w17))) w17->PostSharedBufferToScript(gShared.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, (L"{\"cap\":" + std::to_wstring(RING_CAP) + L"}").c_str());
    return replyNow(id, L"true");
  }
  if (m == L"info") return replyNow(id, std::wstring(L"{\"version\":\"") + LC_VERSION + L"\",\"lite\":true,\"webview2\":" + jstr(gBrowserVer) + L",\"chrome\":" + jstr(gBrowserVer) + L",\"url\":" + jstr(gUrl) + L",\"packaged\":true,\"hwAccel\":" + (gHwAccel ? L"true" : L"false") + L"}");
  if (m == L"winState") return replyNow(id, winStateJson());
  if (m == L"metrics") return replyNow(id, metricsJson());
  if (m == L"setLive") { setLive(arg(0) == L"1"); return replyNow(id, L"true"); }
  if (m == L"getCfg") return replyNow(id, gHwAccel ? L"{\"hwAccel\":true}" : L"{\"hwAccel\":false}");
  if (m == L"setCfg") { if (arg(0) == L"hwAccel") { gHwAccel = arg(1) != L"0"; saveCfg(); } return replyNow(id, gHwAccel ? L"{\"hwAccel\":true}" : L"{\"hwAccel\":false}"); }
  if (m == L"relaunch") { replyNow(id, L"true"); SetTimer(gWnd, 99, 200, [](HWND, UINT, UINT_PTR t, DWORD) { KillTimer(gWnd, t); relaunch(); }); return; }
  if (m == L"audioStart") {
    std::wstring mode = arg(0), pid = arg(1);
    if (mode != L"app" && mode != L"except" && mode != L"all") return failNow(id, L"bad mode");
    if (mode != L"all" && (!digits(pid) || pid == L"0")) return failNow(id, L"没有指定程序");
    return startAudio(mode, pid) ? replyNow(id, std::to_wstring(gAudioGen)) : failNow(id, L"声音助手启动失败");
  }
  if (m == L"audioStop") { stopAudio(); return replyNow(id, L"true"); }
  if (m == L"videoStart") { std::wstring why; std::vector<std::wstring> kv(a.begin() + 2, a.end()); return startVideo(kv, why) ? replyNow(id, std::to_wstring(gVideoGen)) : failNow(id, why); }
  if (m == L"videoStop") { stopVideo(); return replyNow(id, L"true"); }
  if (m == L"videoCmd") return videoCmd(arg(0)) ? replyNow(id, L"true") : failNow(id, L"bad command");
  // 下面几个要跑一会儿，放到别的线程
  if (m == L"screens") { int tw = std::min(640, std::max(0, _wtoi(arg(0).c_str()))); std::thread([id, tw] { replyAsync(id, screensJson(tw)); }).detach(); return; }
  if (m == L"icon") { std::wstring p = arg(0); std::thread([id, p] { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); replyAsync(id, iconJson(p)); CoUninitialize(); }).detach(); return; }
  if (m == L"audioList") {
    std::wstring own = L"["; bool first = true; for (auto& [pid, type] : ownProcs()) { own += (first ? L"" : L",") + std::to_wstring(pid); first = false; } own += L"]";
    std::thread([id, own] {
      std::string out = runCapture(L"\"" + helperPath(L"lc-audio.exe") + L"\" list", 5000);
      while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
      if (out.empty() || out[0] != '{') return replyAsync(id, L"声音助手没有返回", true);
      replyAsync(id, L"{\"info\":" + widen(out) + L",\"own\":" + own + L"}");
    }).detach();
    return;
  }
  failNow(id, L"unknown method");
}

// ---------------- WebView2 ----------------
static void resizeWeb() { if (!gCtl || !gWnd) return; RECT r; GetClientRect(gWnd, &r); gCtl->put_Bounds(r); }
static void openExternal(const std::wstring& uri) { if (startsWith(uri, L"http://") || startsWith(uri, L"https://")) ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
static int gEnvTries = 0;
static void createWeb();
static void fatal(const wchar_t* text) { if (!gHidden) MessageBoxW(gWnd, text, L"LiveCord 轻量版", MB_ICONERROR | MB_OK); DestroyWindow(gWnd); }
static HRESULT onController(HRESULT hr, ICoreWebView2Controller* ctl) {
  if (FAILED(hr) || !ctl) { fatal(L"界面内核（WebView2）启动失败。可以改装完整版。"); return S_OK; }
  gCtl = ctl; gCtl->get_CoreWebView2(&gWeb);
  ComPtr<ICoreWebView2Controller2> c2; if (SUCCEEDED(gCtl.As(&c2))) c2->put_DefaultBackgroundColor({ 255, 0x1e, 0x1f, 0x22 });
  ComPtr<ICoreWebView2Settings> st; gWeb->get_Settings(&st);
  if (st) {
    st->put_AreDefaultContextMenusEnabled(FALSE); st->put_IsStatusBarEnabled(FALSE); st->put_IsZoomControlEnabled(FALSE); st->put_IsBuiltInErrorPageEnabled(FALSE); st->put_AreDevToolsEnabled(TRUE);
    ComPtr<ICoreWebView2Settings3> s3; if (SUCCEEDED(st.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    ComPtr<ICoreWebView2Settings4> s4; if (SUCCEEDED(st.As(&s4))) { s4->put_IsGeneralAutofillEnabled(FALSE); s4->put_IsPasswordAutosaveEnabled(FALSE); }
    ComPtr<ICoreWebView2Settings5> s5; if (SUCCEEDED(st.As(&s5))) s5->put_IsPinchZoomEnabled(FALSE);
    ComPtr<ICoreWebView2Settings6> s6; if (SUCCEEDED(st.As(&s6))) s6->put_IsSwipeNavigationEnabled(FALSE);
  }
  ComPtr<ICoreWebView2Environment12> e12;
  if (SUCCEEDED(gEnv.As(&e12)) && SUCCEEDED(e12->CreateSharedBuffer(RING_HDR + RING_CAP, &gShared)) && gShared) { gShared->get_Buffer(&gRing); if (gRing) memset(gRing, 0, RING_HDR); }
  EventRegistrationToken tk;
  // 只在本站内跳转；其他链接交给系统浏览器
  gWeb->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* e) -> HRESULT {
    LPWSTR u = nullptr; e->get_Uri(&u); std::wstring uri = u ? u : L""; CoTaskMemFree(u);
    if (fromApp(uri)) { gOffline = false; setLive(false); stopAudio(); stopVideo(); } // 换页面了：上一页的直播和助手都不算数
    else if (startsWith(uri, L"data:") || startsWith(uri, L"about:")) { setLive(false); stopAudio(); stopVideo(); }
    else { e->put_Cancel(TRUE); openExternal(uri); }
    return S_OK;
  }).Get(), &tk);
  gWeb->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* e) -> HRESULT {
    LPWSTR u = nullptr; e->get_Uri(&u); e->put_Handled(TRUE); if (u) openExternal(u); CoTaskMemFree(u); return S_OK;
  }).Get(), &tk);
  gWeb->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* e) -> HRESULT {
    BOOL ok = TRUE; COREWEBVIEW2_WEB_ERROR_STATUS s{}; e->get_IsSuccess(&ok); e->get_WebErrorStatus(&s);
    if (!ok && !gOffline && s != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) showOffline(L"错误代码 " + std::to_wstring((int)s));
    return S_OK;
  }).Get(), &tk);
  // 权限：只有本站页面能用摄像头 / 麦克风 / 读剪贴板；系统通知之类一律不给
  gWeb->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* e) -> HRESULT {
    LPWSTR u = nullptr; e->get_Uri(&u); COREWEBVIEW2_PERMISSION_KIND k{}; e->get_PermissionKind(&k);
    bool allow = u && fromApp(u) && (k == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE || k == COREWEBVIEW2_PERMISSION_KIND_CAMERA || k == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ);
    CoTaskMemFree(u); e->put_State(allow ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK;
  }).Get(), &tk);
  gWeb->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* e) -> HRESULT {
    LPWSTR src = nullptr, msg = nullptr; e->get_Source(&src);
    if (SUCCEEDED(e->TryGetWebMessageAsString(&msg)) && msg) onMessage(src ? src : L"", msg);
    CoTaskMemFree(src); CoTaskMemFree(msg); return S_OK;
  }).Get(), &tk);
  gWeb->add_ContainsFullScreenElementChanged(Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>([](ICoreWebView2* w, IUnknown*) -> HRESULT {
    BOOL f = FALSE; w->get_ContainsFullScreenElement(&f); setFullscreen(!!f); return S_OK;
  }).Get(), &tk);
  gWeb->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* e) -> HRESULT {
    COREWEBVIEW2_PROCESS_FAILED_KIND k{}; e->get_ProcessFailedKind(&k);
    if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) { setLive(false); stopAudio(); stopVideo(); relaunch(); }
    else if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) { setLive(false); stopAudio(); stopVideo(); SetTimer(gWnd, TIMER_RELOAD, 500, nullptr); }
    return S_OK;
  }).Get(), &tk);
  gCtl->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>([](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* e) -> HRESULT {
    COREWEBVIEW2_KEY_EVENT_KIND k{}; UINT vk = 0; e->get_KeyEventKind(&k); e->get_VirtualKey(&vk);
    if (k != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && k != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (vk == VK_F12 || (ctrl && shift && vk == 'I')) { gWeb->OpenDevToolsWindow(); e->put_Handled(TRUE); }
    else if (vk == VK_F5 || (ctrl && vk == 'R')) { if (!gLive) { if (gOffline) { gOffline = false; gWeb->Navigate(gUrl.c_str()); } else gWeb->Reload(); } e->put_Handled(TRUE); } // 直播中不让误触刷新
    return S_OK;
  }).Get(), &tk);
  std::wstring shim = widen(resourceText(101)); replaceAll(shim, L"__LC_ORIGIN__", gOrigin);
  gWeb->AddScriptToExecuteOnDocumentCreated(shim.c_str(), Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([](HRESULT, LPCWSTR) -> HRESULT {
    resizeWeb(); gCtl->put_IsVisible(TRUE); gWeb->Navigate(gUrl.c_str()); return S_OK;
  }).Get());
  return S_OK;
}
static void createWeb() {
  // 内核启动参数（和完整版 main.js 里的一一对应）
  std::wstring args = L"--disable-features=D3D12VideoEncodeAccelerator,HardwareMediaKeyHandling,MediaSessionService,CalculateNativeWinOcclusion"
    L" --autoplay-policy=no-user-gesture-required --force-fieldtrials=WebRTC-LegacySimulcastLayerLimit/Disabled/"
    L" --disable-background-timer-throttling --disable-renderer-backgrounding --disable-backgrounding-occluded-windows"; // 窗口最小化 / 被游戏挡住时不能降速
  if (!gHwAccel) args += L" --disable-gpu";
  if (!gDebugPort.empty()) args += L" --remote-debugging-port=" + gDebugPort;
  auto opts = Make<CoreWebView2EnvironmentOptions>(); opts->put_AdditionalBrowserArguments(args.c_str());
  std::wstring data = gDataDir + L"\\WebView2";
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), opts.Get(), Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
    if (FAILED(hr) || !env) {
      // 刚重启时上一个实例的内核可能还没退干净（启动参数不一样就起不来）：等一等再试
      if (++gEnvTries <= 12) { SetTimer(gWnd, 98, 500, [](HWND, UINT, UINT_PTR t, DWORD) { KillTimer(gWnd, t); createWeb(); }); return S_OK; }
      fatal(L"界面内核（WebView2）启动失败。可以改装完整版。"); return S_OK;
    }
    gEnv = env; LPWSTR v = nullptr; if (SUCCEEDED(env->get_BrowserVersionString(&v)) && v) { gBrowserVer = v; CoTaskMemFree(v); }
    env->CreateCoreWebView2Controller(gWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(onController).Get());
    return S_OK;
  }).Get());
  if (FAILED(hr)) fatal(L"这台电脑上没有 WebView2 运行时（Windows 11 自带；Windows 10 需要另外安装）。\n\n可以改装 LiveCord 完整版，它自带内核。");
}

// ---------------- 窗口 ----------------
static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_APP_NOTES: flushNotes(); return 0;
    case WM_APP_JSON: { auto* s = (std::wstring*)lp; postJson(*s); delete s; return 0; }
    case WM_APP_REPLY: { auto* r = (Reply*)lp; if (r->err) failNow(r->id, r->body); else replyNow(r->id, r->body); delete r; return 0; }
    case WM_TIMER: if (wp == TIMER_RELOAD) { KillTimer(h, wp); if (gWeb) gWeb->Reload(); } return 0;
    case WM_SIZE: resizeWeb(); if (gWeb) sendWinState(); if (wp != SIZE_MINIMIZED) rememberBounds(); return 0;
    case WM_MOVE: if (gCtl) gCtl->NotifyParentWindowPositionChanged(); return 0;
    case WM_EXITSIZEMOVE: rememberBounds(); saveCfg(); return 0;
    case WM_ACTIVATE: if (gWeb) sendWinState(); if (LOWORD(wp) != WA_INACTIVE && gCtl) gCtl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); return 0;
    case WM_SETFOCUS: if (gCtl) gCtl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); return 0;
    case WM_SHOWWINDOW: if (gWeb) sendWinState(); break;
    case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; UINT dpi = GetDpiForWindow(h); mm->ptMinTrackSize.x = MulDiv(940, dpi, 96); mm->ptMinTrackSize.y = MulDiv(560, dpi, 96); return 0; }
    case WM_ERASEBKGND: { RECT r; GetClientRect(h, &r); HBRUSH b = CreateSolidBrush(RGB(0x1e, 0x1f, 0x22)); FillRect((HDC)wp, &r, b); DeleteObject(b); return 1; }
    case WM_CLOSE:
      if (gLive && !gHidden && MessageBoxW(h, L"正在直播，确定要退出吗？\n\n退出后直播会立刻结束。", L"LiveCord", MB_ICONQUESTION | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK) return 0;
      rememberBounds(); saveCfg(); DestroyWindow(h); return 0;
    case WM_DESTROY:
      stopAudio(); stopVideo(); SetThreadExecutionState(ES_CONTINUOUS);
      if (gCtl) { gCtl->Close(); gCtl.Reset(); } gWeb.Reset(); gShared.Reset(); gRing = nullptr; gEnv.Reset();
      PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  gInst = inst; SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); gExeDir = exe; gExeDir.erase(gExeDir.find_last_of(L'\\'));
  // 命令行：--lc-url=（只认本机地址，装好的软件不会被一条命令行带去别的网站）、--lc-hidden（自动化测试用，窗口放到屏幕外、不抢焦点）
  gUrl = PROD_URL; DWORD waitPid = 0; int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; i++) {
    std::wstring a = argv[i];
    if (a == L"--lc-hidden") gHidden = true;
    else if (a == L"--lc-noactivate") gNoActivate = true; // 正常显示窗口但不抢焦点（测试用）
    else if (startsWith(a, L"--lc-wait=")) waitPid = wcstoul(a.c_str() + 10, nullptr, 10);
    else if (startsWith(a, L"--remote-debugging-port=") && digits(a.substr(24))) gDebugPort = a.substr(24);
    else if (startsWith(a, L"--lc-url=")) {
      std::wstring u = a.substr(9);
      for (const wchar_t* ok : { L"http://localhost", L"http://127.0.0.1", L"https://localhost", L"https://127.0.0.1" }) {
        size_t n = wcslen(ok); if (!startsWith(u, ok)) continue;
        size_t e = n; if (e < u.size() && u[e] == L':') { e++; while (e < u.size() && iswdigit(u[e])) e++; }
        if (e == u.size() || u[e] == L'/') { gUrl = u.substr(0, e); break; }
      }
    }
  }
  LocalFree(argv);
  gOrigin = gUrl;
  gDataDir = envVar(L"LC_USER_DATA"); // 测试时用独立的数据目录
  if (gDataDir.empty()) { wchar_t* p = nullptr; SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p); gDataDir = std::wstring(p ? p : L".") + L"\\LiveCord Lite"; CoTaskMemFree(p); }
  SHCreateDirectoryExW(nullptr, gDataDir.c_str(), nullptr);
  if (waitPid) { HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, waitPid); if (h) { WaitForSingleObject(h, 8000); CloseHandle(h); } }
  // 只开一个（按数据目录分）：再开一次就把已有的窗口叫出来
  size_t hash = std::hash<std::wstring>{}(gDataDir); std::wstring tag = L"LiveCordLite-" + std::to_wstring(hash);
  gMutex = CreateMutexW(nullptr, TRUE, tag.c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) { HWND w = FindWindowW(tag.c_str(), nullptr); if (w && !gHidden) { if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); } return 0; }
  loadCfg();
  gJob = CreateJobObjectW(nullptr, nullptr);
  if (gJob) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(gJob, JobObjectExtendedLimitInformation, &li, sizeof(li)); }
  Gdiplus::GdiplusStartupInput gsi; ULONG_PTR gtok = 0; Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr);

  WNDCLASSEXW wc{ sizeof(wc) }; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = tag.c_str(); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  RegisterClassExW(&wc);
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 800;
  if (gCfg.have) {
    RECT r{ gCfg.x, gCfg.y, gCfg.x + gCfg.w, gCfg.y + gCfg.h };
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) { x = gCfg.x; y = gCfg.y; } // 上次那块屏幕还在才用上次的位置
    w = gCfg.w; h = gCfg.h;
  }
  if (gHidden) { x = -32000; y = -32000; w = 1280; h = 800; }
  gWnd = CreateWindowExW(gHidden ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, tag.c_str(), L"LiveCord 轻量版", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, nullptr);
  if (!gWnd) return 1;
  // 标题栏跟界面一个颜色（Windows 11；旧系统上这几句不起作用，不影响使用）
  BOOL dark = TRUE; DwmSetWindowAttribute(gWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
  COLORREF cap = RGB(0x1e, 0x1f, 0x22); DwmSetWindowAttribute(gWnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap)); DwmSetWindowAttribute(gWnd, 34 /* DWMWA_BORDER_COLOR */, &cap, sizeof(cap));
  ShowWindow(gWnd, gHidden || gNoActivate ? SW_SHOWNOACTIVATE : (gCfg.max ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL)); UpdateWindow(gWnd);
  createWeb();
  MSG m; while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
  if (gtok) Gdiplus::GdiplusShutdown(gtok);
  if (gMutex) CloseHandle(gMutex);
  CoUninitialize();
  return 0;
}
