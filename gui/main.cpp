#include "shell.h"

#include <windows.h>

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR lpCmdLine, int cmdShow) {
  // --screenshot: capture the UI after load and exit (used for remote debug).
  bool screenshotAfterLoad = false;
  // --restore: restore the saved monitor layout after startup (used by the
  // scheduled task created from the "自恢复" page).
  bool restoreOnStart = false;
  {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
      for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--screenshot") == 0) screenshotAfterLoad = true;
        if (wcscmp(argv[i], L"--restore") == 0) restoreOnStart = true;
      }
      LocalFree(argv);
    }
  }

  // Single instance guard
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"VirtualDisplay.GUI.SingleInstance");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindWindowW(L"VdMainWindow", L"VirtualDisplay");
    if (existing) {
      ShowWindow(existing, SW_RESTORE);
      SetForegroundWindow(existing);
    }
    CloseHandle(mutex);
    return 0;
  }

  // Per-monitor DPI awareness
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  vdgui::Shell shell(screenshotAfterLoad, restoreOnStart);
  return shell.Run(instance, cmdShow);
}
