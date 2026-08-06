#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include <webview2.h>

namespace vdgui {

/**
 * @brief C++ shell: main window hosting a WebView2 control.
 *
 * UI is pure HTML/CSS/JS. JS talks to this shell through WebMessage:
 *   JS -> C++: {id, cmd, args:[...]}  (cmd = iddctrl command)
 *   C++ -> JS: {id, ok, data, error}
 * The shell executes iddctrl via RunCtlJson and returns the raw JSON.
 */
class Shell {
 public:
  explicit Shell(bool screenshotAfterLoad = false, bool restoreOnStart = false);
  ~Shell();

  int Run(HINSTANCE instance, int cmdShow);

  // Called by the COM callback helpers (anonymous namespace in shell.cpp).
  void OnEnvironmentReady(ICoreWebView2Environment *env);
  void OnControllerReady(ICoreWebView2Controller *controller);
  void OnWebMessageReceived(ICoreWebView2WebMessageReceivedEventArgs *args);
  void OnNavigationCompleted(HRESULT result);

 private:
  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;

  ICoreWebView2Environment *environment_ = nullptr;
  ICoreWebView2Controller *controller_ = nullptr;
  ICoreWebView2 *webview_ = nullptr;

  bool screenshotAfterLoad_ = false;
  bool shotTaken_ = false;
  bool restoreOnStart_ = false;
  bool restoreTried_ = false;
  EventRegistrationToken navToken_ = {};

  NOTIFYICONDATAW trayIcon_ = {};
  bool trayAdded_ = false;
  bool trayEnabled_ = true;

  void CreateMainWindow(int cmdShow);
  void InitWebView2();
  void ExecuteCommand(const std::wstring &payload);
  void CreateTray();
  void RemoveTray();
  void ShowTrayMenu();
  void HandleMenuCommand(int id);
  void QuickAdd(int index);
  void NotifyJsRefresh();
  void PostReply(long long id, bool ok, const std::wstring &data, const std::wstring &error);

  static bool ReadTrayEnabled();
  static void WriteTrayEnabled(bool enabled);

  static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
};

}  // namespace vdgui
