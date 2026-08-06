#include "shell.h"

#include "json.h"
#include "resource.h"
#include "strutil.h"
#include "vdctl.h"

#include <commctrl.h>
#include <cstdio>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace vdgui {

namespace {
const wchar_t kMainClass[] = L"VdMainWindow";

constexpr int kDesignW = 1280;
constexpr int kDesignH = 800;

bool g_classInit = false;
ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusInit = false;

void GuiLog(const std::string &line) {
  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
  std::wstring path(modulePath);
  const auto slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos) {
    path.resize(slash + 1);
  }
  path += L"gui.log";
  FILE *f = _wfopen(path.c_str(), L"a");
  if (!f) return;
  SYSTEMTIME st = {};
  GetLocalTime(&st);
  fprintf(f, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute, st.wSecond,
          st.wMilliseconds, line.c_str());
  fclose(f);
}

void EnsureGdiplus() {
  if (g_gdiplusInit) return;
  g_gdiplusInit = true;
  Gdiplus::GdiplusStartupInput input;
  Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

bool SaveWindowPng(HWND hwnd, const std::wstring &path) {
  EnsureGdiplus();
  RECT rc = {};
  if (!GetWindowRect(hwnd, &rc)) return false;
  int w = rc.right - rc.left;
  int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return false;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);

  BOOL ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);

  Gdiplus::Bitmap gbmp(bmp, nullptr);
  CLSID pngClsid;
  // {557CF406-1A04-11D3-9A73-0000F81EF32E} = image/png encoder
  pngClsid.Data1 = 0x557CF406; pngClsid.Data2 = 0x1A04; pngClsid.Data3 = 0x11D3;
  pngClsid.Data4[0] = 0x9A; pngClsid.Data4[1] = 0x73; pngClsid.Data4[2] = 0x00;
  pngClsid.Data4[3] = 0x00; pngClsid.Data4[4] = 0xF8; pngClsid.Data4[5] = 0x1E;
  pngClsid.Data4[6] = 0xF3; pngClsid.Data4[7] = 0x2E;
  Gdiplus::Status st = gbmp.Save(path.c_str(), &pngClsid, nullptr);

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok && st == Gdiplus::Ok;
}

bool SaveScreenPng(const std::wstring &path) {
  EnsureGdiplus();
  int w = GetSystemMetrics(SM_CXSCREEN);
  int h = GetSystemMetrics(SM_CYSCREEN);
  if (w <= 0 || h <= 0) return false;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  BOOL ok = BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY);

  Gdiplus::Bitmap gbmp(bmp, nullptr);
  CLSID pngClsid;
  pngClsid.Data1 = 0x557CF406; pngClsid.Data2 = 0x1A04; pngClsid.Data3 = 0x11D3;
  pngClsid.Data4[0] = 0x9A; pngClsid.Data4[1] = 0x73; pngClsid.Data4[2] = 0x00;
  pngClsid.Data4[3] = 0x00; pngClsid.Data4[4] = 0xF8; pngClsid.Data4[5] = 0x1E;
  pngClsid.Data4[6] = 0xF3; pngClsid.Data4[7] = 0x2E;
  Gdiplus::Status st = gbmp.Save(path.c_str(), &pngClsid, nullptr);

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok && st == Gdiplus::Ok;
}

void InitCommon() {
  if (g_classInit) return;
  g_classInit = true;
  INITCOMMONCONTROLSEX icc = {};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
  InitCommonControlsEx(&icc);
}

std::wstring UiFolder() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  PathRemoveFileSpecW(path);
  std::wstring dir(path);
  dir += L"\\ui";
  return dir;
}

std::wstring JsonEscape(const std::wstring &s) {
  std::wstring out;
  out.reserve(s.size() + 8);
  for (wchar_t c : s) {
    switch (c) {
      case L'"': out += L"\\\""; break;
      case L'\\': out += L"\\\\"; break;
      case L'\n': out += L"\\n"; break;
      case L'\r': out += L"\\r"; break;
      case L'\t': out += L"\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string HexStr(uint32_t v) {
  char buf[16] = {};
  snprintf(buf, sizeof(buf), "%08X", v);
  return buf;
}

// ---------------------------------------------------------------------------
// Manual COM callback implementations (no WRL dependency).
// ---------------------------------------------------------------------------
class EnvironmentHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
 public:
  explicit EnvironmentHandler(Shell *shell) : shell_(shell), ref_(1) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown ||
        riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override {
    if (FAILED(result)) return result;
    if (!env) return E_FAIL;
    shell_->OnEnvironmentReady(env);
    return S_OK;
  }

 private:
  Shell *shell_;
  ULONG ref_;
};

class ControllerHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
 public:
  explicit ControllerHandler(Shell *shell) : shell_(shell), ref_(1) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown ||
        riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override {
    if (FAILED(result)) return result;
    if (!controller) return E_FAIL;
    shell_->OnControllerReady(controller);
    return S_OK;
  }

 private:
  Shell *shell_;
  ULONG ref_;
};

class MessageHandler final : public ICoreWebView2WebMessageReceivedEventHandler {
 public:
  explicit MessageHandler(Shell *shell) : shell_(shell), ref_(1) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                   ICoreWebView2WebMessageReceivedEventArgs *args) override {
    (void)sender;
    shell_->OnWebMessageReceived(args);
    return S_OK;
  }

 private:
  Shell *shell_;
  ULONG ref_;
};

class NavigationHandler final : public ICoreWebView2NavigationCompletedEventHandler {
 public:
  explicit NavigationHandler(Shell *shell) : shell_(shell), ref_(1) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (riid == IID_IUnknown ||
        riid == __uuidof(ICoreWebView2NavigationCompletedEventHandler)) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                   ICoreWebView2NavigationCompletedEventArgs *args) override {
    (void)sender;
    BOOL success = FALSE;
    args->get_IsSuccess(&success);
    shell_->OnNavigationCompleted(success ? S_OK : E_FAIL);
    return S_OK;
  }

 private:
  Shell *shell_;
  ULONG ref_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Shell (implementation split so the handlers can call friend-free methods)
// ---------------------------------------------------------------------------
Shell::Shell(bool screenshotAfterLoad, bool restoreOnStart)
  : screenshotAfterLoad_(screenshotAfterLoad), restoreOnStart_(restoreOnStart) {}

Shell::~Shell() {
  RemoveTray();
  if (webview_) webview_->Release();
  if (controller_) controller_->Release();
  if (environment_) environment_->Release();
}

int Shell::Run(HINSTANCE instance, int cmdShow) {
  instance_ = instance;
  InitCommon();

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProcStatic;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kMainClass;
  RegisterClassExW(&wc);

  CreateMainWindow(cmdShow);
  if (!hwnd_) return 1;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (SUCCEEDED(hr)) {
    InitWebView2();
  }

  MSG msg = {};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

void Shell::CreateMainWindow(int cmdShow) {
  int clientW = MulDiv(kDesignW, GetDpiForSystem(), 96);
  int clientH = MulDiv(kDesignH, GetDpiForSystem(), 96);
  // Borderless window: the WebView2 UI renders a custom title bar
  // (drag region + minimize/close buttons).
  RECT winRect = {0, 0, clientW, clientH};
  int w = winRect.right - winRect.left;
  int h = winRect.bottom - winRect.top;

  RECT work = {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int x = work.left + (work.right - work.left - w) / 2;
  int y = work.top + (work.bottom - work.top - h) / 2;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;

  hwnd_ = CreateWindowExW(0, kMainClass, L"VirtualDisplay",
                          WS_POPUP | WS_CLIPCHILDREN, x, y, w, h,
                          nullptr, nullptr, instance_, this);
  if (!hwnd_) return;

  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));

  // Force a proper show state: ignore suspicious cmdShow values (e.g. 0/SW_HIDE
  // when launched from a service/injection) and always display the window.
  if (cmdShow == SW_HIDE) cmdShow = SW_SHOW;
  ShowWindow(hwnd_, cmdShow);
  UpdateWindow(hwnd_);
  trayEnabled_ = ReadTrayEnabled();
  if (trayEnabled_) {
    CreateTray();
  }
}

void Shell::InitWebView2() {
  // Use a user-data folder under LocalAppData (not next to the exe).
  std::wstring userDataFolder;
  {
    wchar_t localAppData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, localAppData))) {
      userDataFolder = localAppData;
      userDataFolder += L"\\VirtualDisplay\\WebView2";
    }
  }

  auto *handler = new EnvironmentHandler(this);
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, userDataFolder.empty() ? nullptr : userDataFolder.c_str(), nullptr, handler);
  if (FAILED(hr)) {
    handler->Release();
  }
}

void Shell::OnEnvironmentReady(ICoreWebView2Environment *env) {
  environment_ = env;
  environment_->AddRef();

  auto *handler = new ControllerHandler(this);
  HRESULT hr = environment_->CreateCoreWebView2Controller(hwnd_, handler);
  if (FAILED(hr)) {
    handler->Release();
  }
}

void Shell::OnControllerReady(ICoreWebView2Controller *controller) {
  controller_ = controller;
  controller_->AddRef();

  controller_->get_CoreWebView2(&webview_);

  RECT rc = {};
  GetClientRect(hwnd_, &rc);
  controller_->put_Bounds(rc);

  auto *msgHandler = new MessageHandler(this);
  webview_->add_WebMessageReceived(msgHandler, nullptr);

  ICoreWebView2Settings *settings = nullptr;
  if (SUCCEEDED(webview_->get_Settings(&settings))) {
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->Release();
  }

  std::wstring uiFolder = UiFolder();
  ICoreWebView2_3 *webview3 = nullptr;
  if (SUCCEEDED(webview_->QueryInterface(__uuidof(ICoreWebView2_3),
                                         reinterpret_cast<void **>(&webview3)))) {
    webview3->SetVirtualHostNameToFolderMapping(
        L"app.local", uiFolder.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    webview3->Release();
  }

  auto *navHandler = new NavigationHandler(this);
  webview_->add_NavigationCompleted(navHandler, &navToken_);
  navHandler->Release();

  webview_->Navigate(L"https://app.local/index.html");
}

void Shell::OnNavigationCompleted(HRESULT result) {
  // Restore the saved monitor layout after startup. Delayed so the driver
  // device is fully up before iddctrl runs; failures are logged, not fatal.
  if (restoreOnStart_ && !restoreTried_) {
    restoreTried_ = true;
    SetTimer(hwnd_, 2, 5000, nullptr);
  }
  if (!screenshotAfterLoad_ || shotTaken_) return;
  shotTaken_ = true;
  SetTimer(hwnd_, 1, 2500, nullptr);
}

void Shell::OnWebMessageReceived(ICoreWebView2WebMessageReceivedEventArgs *args) {
  PWSTR raw = nullptr;
  if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return;
  std::wstring wpayload(raw);
  CoTaskMemFree(raw);

  std::string payload = WideToUtf8(wpayload);
  bool ok = false;
  JsonValue msg = JsonParse(payload, &ok);
  if (!ok || !msg.IsObject()) return;

  const std::string cmd = msg.Str("cmd");
  if (cmd.empty()) return;
  long long id = msg.Int("id", 0);

  // Special commands handled natively.
  if (cmd == "openlog") {
    ShellExecuteW(nullptr, L"open", L"notepad.exe",
                  L"C:\\ProgramData\\VirtualDisplay\\VirtualDisplay.log", nullptr, SW_SHOW);
    PostReply(id, true, L"", L"");
    return;
  }

  if (cmd == "--drag") {
    // Classic Win32 trick: release capture and send a caption hit-test so the
    // system moves the window following the mouse (used by the custom title
    // bar drag region).
    ReleaseCapture();
    SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    PostReply(id, true, L"", L"");
    return;
  }

  if (cmd == "--minimize") {
    ShowWindow(hwnd_, SW_MINIMIZE);
    PostReply(id, true, L"", L"");
    return;
  }

  if (cmd == "--close") {
    DestroyWindow(hwnd_);
    PostReply(id, true, L"", L"");
    return;
  }

  if (cmd == "screenshot") {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    PathRemoveFileSpecW(path);
    std::wstring png = path;
    png += L"\\VirtualDisplay_shot.png";
    bool saved = SaveWindowPng(hwnd_, png);
    PostReply(id, saved, saved ? L"" : L"save failed", saved ? L"" : L"");
    return;
  }

  if (cmd == "settray") {
    // args[0] = "0" (off) or "1" (on). Persists and applies immediately.
    bool enable = true;
    const JsonValue &arr = msg.Get("args");
    if (arr.IsArray() && arr.arrayValue.size() > 0) {
      const JsonValue &v = arr.arrayValue[0];
      if (v.IsString() && v.stringValue == "0") enable = false;
      if (v.IsNumber() && v.numberValue == 0) enable = false;
    }
    trayEnabled_ = enable;
    WriteTrayEnabled(enable);
    if (enable && !trayAdded_) {
      CreateTray();
    } else if (!enable && trayAdded_) {
      RemoveTray();
    }
    PostReply(id, true, L"", L"");
    return;
  }

  if (cmd == "tray-status") {
    PostReply(id, true, trayEnabled_ ? L"1" : L"0", L"");
    return;
  }

  std::vector<std::string> ctlArgs;
  const JsonValue &arr = msg.Get("args");
  if (arr.IsArray()) {
    for (const auto &e : arr.arrayValue) {
      // Array elements are plain values, not objects: Str(key, dflt) would
      // always return the default (""). Extract the scalar value directly.
      if (e.IsString()) {
        ctlArgs.push_back(e.stringValue);
      } else if (e.IsNumber()) {
        ctlArgs.push_back(std::to_string(static_cast<long long>(e.numberValue)));
      }
    }
  }
  ctlArgs.insert(ctlArgs.begin(), cmd);

  std::string logLine = "CTL ";
  for (const auto &a : ctlArgs) {
    logLine += "[" + a + "] ";
  }
  GuiLog(logLine);

  CtlJson result = RunCtlJson(ctlArgs);
  GuiLog("CTL done exit=" + std::to_string(result.exitCode) +
         " ok=" + (result.ok ? "1" : "0") + " bytes=" +
         std::to_string(result.raw.size()));
  if (result.ok) {
    if (result.validJson) {
      PostReply(id, true, Utf8ToWide(result.raw), L"");
    } else {
      // Non-JSON output (e.g. uninstall progress text): deliver it as a
      // quoted string so PostWebMessageAsJson never receives invalid JSON.
      PostReply(id, true, L"", Utf8ToWide(result.raw));
    }
  } else {
    PostReply(id, false, L"", Utf8ToWide(result.raw));
  }
}

void Shell::PostReply(long long id, bool ok, const std::wstring &data, const std::wstring &error) {
  if (!webview_) return;
  std::wstring reply = L"{\"id\":" + std::to_wstring(id) + L",\"ok\":";
  reply += (ok ? L"true" : L"false");
  // Always deliver data/error as quoted, escaped strings. Raw JSON is only
  // transported inside a string; the JS side JSON.parse()s it when needed.
  // This guarantees PostWebMessageAsJson always receives valid JSON.
  reply += L",\"data\":\"" + JsonEscape(data) + L"\"";
  if (!error.empty()) {
    reply += L",\"error\":\"" + JsonEscape(error) + L"\"";
  }
  reply += L"}";
  HRESULT hr = webview_->PostWebMessageAsJson(reply.c_str());
  GuiLog("PostReply id=" + std::to_string(id) + " ok=" + (ok ? "1" : "0") +
         " reply_bytes=" + std::to_string(reply.size()) + " hr=0x" +
         HexStr(static_cast<uint32_t>(hr)));
}

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------
bool Shell::ReadTrayEnabled() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\VirtualDisplay",
                   L"TrayEnabled",
                   RRF_RT_REG_DWORD,
                   nullptr,
                   &value,
                   &size) == ERROR_SUCCESS) {
    return value != 0;
  }
  return true;  // default: tray enabled
}

void Shell::WriteTrayEnabled(bool enabled) {
  DWORD value = enabled ? 1 : 0;
  RegSetKeyValueW(HKEY_CURRENT_USER,
                  L"Software\\VirtualDisplay",
                  L"TrayEnabled",
                  REG_DWORD,
                  &value,
                  sizeof(value));
}

void Shell::CreateTray() {
  trayIcon_.cbSize = NOTIFYICONDATAW_V2_SIZE;
  trayIcon_.hWnd = hwnd_;
  trayIcon_.uID = 1;
  trayIcon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  trayIcon_.uCallbackMessage = WM_APP + 1;
  trayIcon_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
  wcscpy_s(trayIcon_.szTip, L"VirtualDisplay");
  Shell_NotifyIconW(NIM_ADD, &trayIcon_);
  trayAdded_ = true;
}

void Shell::RemoveTray() {
  if (trayAdded_) {
    Shell_NotifyIconW(NIM_DELETE, &trayIcon_);
    trayAdded_ = false;
  }
}

void Shell::ShowTrayMenu() {
  SetForegroundWindow(hwnd_);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 200, L"\u6253\u5f00 VirtualDisplay");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 201, L"\u5feb\u901f\u6dfb\u52a0 1080@60");
  AppendMenuW(menu, MF_STRING, 202, L"\u5feb\u901f\u6dfb\u52a0 1080@120");
  AppendMenuW(menu, MF_STRING, 203, L"\u5feb\u901f\u6dfb\u52a0 2K@60");
  AppendMenuW(menu, MF_STRING, 204, L"\u5feb\u901f\u6dfb\u52a0 2K@120");
  AppendMenuW(menu, MF_STRING, 205, L"\u5feb\u901f\u6dfb\u52a0 4K@60");
  AppendMenuW(menu, MF_STRING, 206, L"\u5feb\u901f\u6dfb\u52a0 4K@120");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 207, L"\u9000\u51fa");
  POINT pt = {};
  GetCursorPos(&pt);
  // TPM_RETURNCMD: the chosen id is the return value; WM_COMMAND is NOT sent.
  const int chosen = static_cast<int>(TrackPopupMenu(
      menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr));
  DestroyMenu(menu);
  if (chosen == 0) return;
  HandleMenuCommand(chosen);
}

void Shell::QuickAdd(int index) {
  static const int kW[6] = {1920, 1920, 2560, 2560, 3840, 3840};
  static const int kH[6] = {1080, 1080, 1440, 1440, 2160, 2160};
  static const int kV[6] = {60000, 120000, 60000, 120000, 60000, 120000};
  if (index < 0 || index >= 6) return;
  RunCtlJson({"add", std::to_string(kW[index]), std::to_string(kH[index]),
              std::to_string(kV[index])});
  NotifyJsRefresh();
}

void Shell::HandleMenuCommand(int id) {
  if (id == 200) {
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
  } else if (id >= 201 && id <= 206) {
    QuickAdd(id - 201);
  } else if (id == 207) {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }
}

void Shell::NotifyJsRefresh() {
  if (webview_) {
    webview_->PostWebMessageAsJson(L"{\"event\":\"refresh\"}");
  }
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------
LRESULT CALLBACK Shell::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Shell *self = reinterpret_cast<Shell *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self && msg == WM_NCCREATE) {
    auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
    self = static_cast<Shell *>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
  return self->WndProc(msg, wp, lp);
}

LRESULT Shell::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE: {
      if (controller_) {
        RECT rc = {};
        GetClientRect(hwnd_, &rc);
        controller_->put_Bounds(rc);
      }
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT *suggested = reinterpret_cast<const RECT *>(lp);
      SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      if (controller_) {
        RECT rc = {};
        GetClientRect(hwnd_, &rc);
        controller_->put_Bounds(rc);
      }
      return 0;
    }
    case WM_NCHITTEST: {
      // Borderless window: provide resize hit zones on the edges.
      RECT rc = {};
      GetWindowRect(hwnd_, &rc);
      constexpr int kEdge = 8;
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      bool left = pt.x < rc.left + kEdge;
      bool right = pt.x >= rc.right - kEdge;
      bool top = pt.y < rc.top + kEdge;
      bool bottom = pt.y >= rc.bottom - kEdge;
      if (top && left) return HTTOPLEFT;
      if (top && right) return HTTOPRIGHT;
      if (bottom && left) return HTBOTTOMLEFT;
      if (bottom && right) return HTBOTTOMRIGHT;
      if (top) return HTTOP;
      if (bottom) return HTBOTTOM;
      if (left) return HTLEFT;
      if (right) return HTRIGHT;
      return HTCLIENT;
    }
    case WM_APP + 1:
      if (LOWORD(lp) == WM_RBUTTONUP) {
        ShowTrayMenu();
      } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
      }
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (id == 200 || (id >= 201 && id <= 207)) {
        HandleMenuCommand(id);
      }
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_MINIMIZE && trayEnabled_) {
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
      }
      break;
    case WM_TIMER:
      if (wp == 1) {
        KillTimer(hwnd_, 1);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        PathRemoveFileSpecW(path);
        std::wstring png = path;
        png += L"\\VirtualDisplay_shot.png";
        SaveWindowPng(hwnd_, png);
        std::wstring screen = path;
        screen += L"\\VirtualDisplay_screen.png";
        SaveScreenPng(screen);
        DestroyWindow(hwnd_);
        return 0;
      }
      if (wp == 2) {
        KillTimer(hwnd_, 2);
        GuiLog("Auto-restore: running iddctrl restore");
        CtlJson result = RunCtlJson({"restore"});
        GuiLog("Auto-restore: exit=" + std::to_string(result.exitCode) +
               " ok=" + (result.ok ? "1" : "0"));
        return 0;
      }
      break;
    case WM_CLOSE:
      if (trayEnabled_) {
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace vdgui
