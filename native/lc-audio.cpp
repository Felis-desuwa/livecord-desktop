// lc-audio：LiveCord 桌面版的声音采集助手（Windows 10 2004 及以上）
//
//   lc-audio list
//       标准输出一行 JSON：{"apps":[...],"windows":[...]}
//       apps    有音频会话的程序，按"程序"归并（同一个程序的多个进程算一个，pid 是最上层那个进程）
//       windows 可见的顶层窗口及其所属程序（用来把"共享的窗口"对应到程序）
//
//   lc-audio gpuprio <0-5> <pid>...
//       设置进程的显卡调度优先级（见 cmdGpuPrio）
//
//   lc-audio capture <pid> [include|exclude]
//       include（默认）：只采集这个进程及其子进程发出的声音
//       exclude        ：采集电脑上除这个进程树以外的全部声音
//       标准输出：48000 Hz、双声道、32 位浮点小端的原始 PCM。目标没出声时不输出数据。
//       标准输入被关闭（父进程退出）时自动结束；include 模式下目标进程退出时以退出码 3 结束。
//
// 用的是 WASAPI 的按进程回环采集（AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK），
// 这是系统级的按进程隔离，和共享哪个画面无关。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <tlhelp32.h>
#include <avrt.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

template <class T> struct Com {
  T* p = nullptr;
  ~Com() { if (p) p->Release(); }
  T* operator->() { return p; }
  T** operator&() { return &p; }
  operator T*() { return p; }
};

static std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
static std::string jstr(const std::wstring& w) {
  std::string in = utf8(w), out = "\"";
  for (unsigned char c : in) {
    if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
    else if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); out += b; }
    else out += (char)c;
  }
  return out + "\"";
}

// ---------------- 进程信息 ----------------
struct ProcInfo { DWORD pid = 0, ppid = 0; std::wstring image; };
static std::map<DWORD, ProcInfo> snapshotProcs() {
  std::map<DWORD, ProcInfo> m;
  HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (h == INVALID_HANDLE_VALUE) return m;
  PROCESSENTRY32W e{ sizeof(e) };
  if (Process32FirstW(h, &e)) do { m[e.th32ProcessID] = { e.th32ProcessID, e.th32ParentProcessID, e.szExeFile }; } while (Process32NextW(h, &e));
  CloseHandle(h);
  return m;
}
static ULONGLONG procStart(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return 0;
  FILETIME c, e, k, u; ULONGLONG t = 0;
  if (GetProcessTimes(h, &c, &e, &k, &u)) t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
  CloseHandle(h);
  return t;
}
// 同一个程序的最上层进程：一路往上找映像名相同的父进程（Discord、浏览器、带启动器的游戏都是多进程）。
// 父进程必须比子进程先启动，防止进程号被复用后认错
static DWORD rootOf(DWORD pid, const std::map<DWORD, ProcInfo>& procs) {
  DWORD cur = pid;
  for (int i = 0; i < 16; i++) {
    auto it = procs.find(cur); if (it == procs.end()) break;
    auto pt = procs.find(it->second.ppid); if (pt == procs.end() || pt->first == 0) break;
    if (_wcsicmp(pt->second.image.c_str(), it->second.image.c_str()) != 0) break;
    ULONGLONG a = procStart(pt->first), b = procStart(cur);
    if (!a || !b || a > b) break;
    cur = pt->first;
  }
  return cur;
}
static std::wstring exePath(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return {};
  wchar_t buf[1024]; DWORD n = 1024; std::wstring s;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) s.assign(buf, n);
  CloseHandle(h);
  return s;
}
// 程序的显示名：可执行文件里的 FileDescription，没有就用文件名
static std::wstring friendlyName(const std::wstring& path, const std::wstring& image) {
  if (!path.empty()) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size) {
      std::vector<BYTE> data(size);
      if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        struct LANGCP { WORD lang, cp; } *tr = nullptr; UINT len = 0;
        if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LANGCP)) {
          wchar_t key[80]; swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
          wchar_t* val = nullptr; UINT vl = 0;
          if (VerQueryValueW(data.data(), key, (void**)&val, &vl) && vl > 1) { std::wstring s(val); if (!s.empty() && s.find_first_not_of(L' ') != std::wstring::npos) return s; }
        }
      }
    }
  }
  std::wstring n = image; size_t dot = n.rfind(L'.'); if (dot != std::wstring::npos) n.resize(dot);
  return n;
}

// ---------------- list ----------------
struct WinInfo { HWND hwnd; DWORD pid; std::wstring title; };
static BOOL CALLBACK enumWin(HWND h, LPARAM lp) {
  if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  LONG ex = GetWindowLongW(h, GWL_EXSTYLE); if (ex & WS_EX_TOOLWINDOW) return TRUE;
  wchar_t t[512]; int n = GetWindowTextW(h, t, 512); if (n <= 0) return TRUE;
  DWORD pid = 0; GetWindowThreadProcessId(h, &pid); if (!pid) return TRUE;
  ((std::vector<WinInfo>*)lp)->push_back({ h, pid, std::wstring(t, n) });
  return TRUE;
}
struct App { DWORD root = 0; std::wstring image, path, name, title; bool active = false; float peak = 0; std::vector<DWORD> pids; };

static int cmdList() {
  auto procs = snapshotProcs();
  std::vector<WinInfo> wins; EnumWindows(enumWin, (LPARAM)&wins);
  std::map<DWORD, App> apps;
  Com<IMMDeviceEnumerator> en;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) {
    Com<IMMDeviceCollection> col;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
      UINT nd = 0; col->GetCount(&nd);
      for (UINT d = 0; d < nd; d++) {
        Com<IMMDevice> dev; if (FAILED(col->Item(d, &dev))) continue;
        Com<IAudioSessionManager2> mgr; if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr))) continue;
        Com<IAudioSessionEnumerator> se; if (FAILED(mgr->GetSessionEnumerator(&se))) continue;
        int ns = 0; se->GetCount(&ns);
        for (int i = 0; i < ns; i++) {
          Com<IAudioSessionControl> sc; if (FAILED(se->GetSession(i, &sc))) continue;
          Com<IAudioSessionControl2> sc2; if (FAILED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2))) continue;
          if (sc2->IsSystemSoundsSession() == S_OK) continue;
          DWORD pid = 0; if (FAILED(sc2->GetProcessId(&pid)) || !pid) continue;
          if (procs.find(pid) == procs.end()) continue; // 进程已经退出，会话还没回收
          AudioSessionState st = AudioSessionStateInactive; sc->GetState(&st);
          float peak = 0; Com<IAudioMeterInformation> meter;
          if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&meter))) meter->GetPeakValue(&peak);
          DWORD root = rootOf(pid, procs);
          App& a = apps[root];
          a.root = root; a.active = a.active || st == AudioSessionStateActive; a.peak = std::max(a.peak, peak);
          if (std::find(a.pids.begin(), a.pids.end(), pid) == a.pids.end()) a.pids.push_back(pid);
        }
      }
    }
  }
  std::string out = "{\"apps\":[";
  bool first = true;
  for (auto& [root, a] : apps) {
    a.image = procs[root].image; a.path = exePath(root); a.name = friendlyName(a.path, a.image);
    for (auto& w : wins) if (rootOf(w.pid, procs) == root) { a.title = w.title; break; }
    char b[96];
    if (!first) out += ","; first = false;
    out += "{\"pid\":" + std::to_string(root) + ",\"name\":" + jstr(a.name) + ",\"image\":" + jstr(a.image) + ",\"path\":" + jstr(a.path) + ",\"title\":" + jstr(a.title);
    sprintf_s(b, ",\"active\":%s,\"peak\":%.4f}", a.active ? "true" : "false", a.peak); out += b;
  }
  out += "],\"windows\":["; first = true;
  for (auto& w : wins) {
    DWORD root = rootOf(w.pid, procs);
    if (!first) out += ","; first = false;
    std::wstring path = exePath(root);
    out += "{\"hwnd\":" + std::to_string((unsigned long long)(UINT_PTR)w.hwnd) + ",\"pid\":" + std::to_string(root) + ",\"title\":" + jstr(w.title)
      + ",\"image\":" + jstr(procs[root].image) + ",\"path\":" + jstr(path) + ",\"name\":" + jstr(friendlyName(path, procs[root].image)) + "}";
  }
  out += "]}\n";
  fwrite(out.data(), 1, out.size(), stdout); fflush(stdout);
  return 0;
}

// ---------------- capture ----------------
struct ActivateHandler : IActivateAudioInterfaceCompletionHandler, IAgileObject {
  LONG ref = 1; HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HRESULT hr = E_FAIL; IAudioClient* client = nullptr;
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    else if (iid == __uuidof(IAgileObject)) *out = static_cast<IAgileObject*>(this);
    else { *out = nullptr; return E_NOINTERFACE; }
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
  STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&ref); } // 生命周期跟着 main，不自删
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
    HRESULT act = E_FAIL; IUnknown* unk = nullptr;
    hr = op->GetActivateResult(&act, &unk);
    if (SUCCEEDED(hr)) hr = act;
    if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), (void**)&client);
    if (unk) unk->Release();
    SetEvent(done);
    return S_OK;
  }
};

static DWORD WINAPI stdinWatch(LPVOID) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE); char b[64]; DWORD n = 0;
  while (ReadFile(in, b, sizeof(b), &n, nullptr) && n > 0) {}
  ExitProcess(0);
}
static int fail(const char* what, HRESULT hr) { fprintf(stderr, "error %s 0x%08lx\n", what, (unsigned long)hr); fflush(stderr); return 2; }

static int cmdCapture(DWORD pid, bool exclude) {
  _setmode(_fileno(stdout), _O_BINARY);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  CreateThread(nullptr, 0, stdinWatch, nullptr, 0, nullptr);

  AUDIOCLIENT_ACTIVATION_PARAMS ap{};
  ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  ap.ProcessLoopbackParams.TargetProcessId = pid;
  ap.ProcessLoopbackParams.ProcessLoopbackMode = exclude ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT pv{}; pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;
  static ActivateHandler handler;
  Com<IActivateAudioInterfaceAsyncOperation> op;
  HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, &handler, &op);
  if (FAILED(hr)) return fail("activate", hr);
  if (WaitForSingleObject(handler.done, 10000) != WAIT_OBJECT_0) return fail("activate-timeout", E_FAIL);
  if (FAILED(handler.hr) || !handler.client) return fail("activate-result", handler.hr);
  IAudioClient* client = handler.client;

  WAVEFORMATEXTENSIBLE wf{};
  wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wf.Format.nChannels = 2; wf.Format.nSamplesPerSec = 48000;
  wf.Format.wBitsPerSample = 32; wf.Format.nBlockAlign = 8; wf.Format.nAvgBytesPerSec = 48000 * 8;
  wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wf.Samples.wValidBitsPerSample = 32; wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, &wf.Format, nullptr);
  if (FAILED(hr)) return fail("initialize", hr);
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(hr = client->SetEventHandle(ev))) return fail("event", hr);
  Com<IAudioCaptureClient> cap;
  if (FAILED(hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) return fail("service", hr);
  DWORD task = 0; AvSetMmThreadCharacteristicsW(L"Audio", &task);
  if (FAILED(hr = client->Start())) return fail("start", hr);
  fprintf(stderr, "ready\n"); fflush(stderr);

  HANDLE target = exclude ? nullptr : OpenProcess(SYNCHRONIZE, FALSE, pid);
  HANDLE waits[2] = { ev, target }; DWORD nw = target ? 2 : 1;
  std::vector<BYTE> zeros;
  for (;;) {
    DWORD w = WaitForMultipleObjects(nw, waits, FALSE, 1000);
    if (w == WAIT_OBJECT_0 + 1) { fprintf(stderr, "target-exit\n"); fflush(stderr); return 3; }
    if (w == WAIT_FAILED) return fail("wait", HRESULT_FROM_WIN32(GetLastError()));
    for (;;) {
      UINT32 frames = 0; hr = cap->GetNextPacketSize(&frames);
      if (FAILED(hr)) return fail("packet", hr);
      if (!frames) break;
      BYTE* data = nullptr; DWORD fl = 0;
      hr = cap->GetBuffer(&data, &frames, &fl, nullptr, nullptr);
      if (FAILED(hr)) return fail("buffer", hr);
      DWORD bytes = frames * 8, wr = 0; BOOL ok;
      if (fl & AUDCLNT_BUFFERFLAGS_SILENT) { if (zeros.size() < bytes) zeros.assign(bytes, 0); ok = WriteFile(out, zeros.data(), bytes, &wr, nullptr); }
      else ok = WriteFile(out, data, bytes, &wr, nullptr);
      cap->ReleaseBuffer(frames);
      if (!ok) return 0; // 读的那头关了
    }
  }
}

// ---------------- gpuprio ----------------
// 显卡调度优先级（和 CPU 优先级是两回事）。游戏把显卡占满时，普通优先级的进程提交的显卡工作（抓屏、编码）会被排在后面；
// 提到"高"以后系统会优先调度。这是每个进程自己的属性，进程退出就没了，不改任何系统设置
static int cmdGpuPrio(int argc, wchar_t** argv) {
  typedef LONG (WINAPI *SetFn)(HANDLE, int); typedef LONG (WINAPI *GetFn)(HANDLE, int*);
  HMODULE gdi = LoadLibraryW(L"gdi32.dll");
  SetFn set = gdi ? (SetFn)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass") : nullptr;
  GetFn get = gdi ? (GetFn)GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass") : nullptr;
  if (!set) { fprintf(stderr, "error no-api\n"); return 2; }
  int cls = _wtoi(argv[2]); if (cls < 0 || cls > 5) return 1; // 0 空闲 1 低于正常 2 正常 3 高于正常 4 高 5 实时
  int rc = 0;
  for (int i = 3; i < argc; i++) {
    DWORD pid = wcstoul(argv[i], nullptr, 10);
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { printf("%lu open-failed %lu\n", pid, GetLastError()); rc = 2; continue; }
    LONG st = set(h, cls); int now = -1; if (get) get(h, &now);
    printf("%lu status=0x%08lx class=%d\n", pid, (unsigned long)st, now);
    if (st != 0) rc = 2;
    CloseHandle(h);
  }
  fflush(stdout);
  return rc;
}

int wmain(int argc, wchar_t** argv) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) return fail("coinit", hr);
  if (argc >= 2 && !wcscmp(argv[1], L"list")) return cmdList();
  if (argc >= 4 && !wcscmp(argv[1], L"gpuprio")) return cmdGpuPrio(argc, argv);
  if (argc >= 3 && !wcscmp(argv[1], L"capture")) {
    DWORD pid = wcstoul(argv[2], nullptr, 10);
    if (!pid) { fprintf(stderr, "error bad-pid\n"); return 2; }
    return cmdCapture(pid, argc >= 4 && !wcscmp(argv[3], L"exclude"));
  }
  fprintf(stderr, "usage: lc-audio list | lc-audio capture <pid> [include|exclude]\n");
  return 1;
}
