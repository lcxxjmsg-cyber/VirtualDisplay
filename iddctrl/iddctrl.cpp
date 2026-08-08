#include "../driver/ioctl.h"

#include <algorithm>
#include <cctype>
#include <cfgmgr32.h>
#include <cstdint>
#include <d3d11.h>
#include <devguid.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <fstream>
#include <iostream>
#include <map>
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <tlhelp32.h>
#include <userenv.h>
#include <vector>
#include <windows.h>
#include <wtsapi32.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

static constexpr UINT32 VSYNC_MHZ_DENOMINATOR = 1000;

/**
 * @brief Check whether Windows test-signing mode is enabled.
 *
 * Test-signed drivers are blocked (ERROR_DRIVER_BLOCKED, 0xE000024B) by the
 * driver store unless `bcdedit /set testsigning on` is active. This is a
 * common cause of install failures that users misread as a permissions issue.
 */
static bool IsTestSigningEnabled() {
  wchar_t systemRoot[MAX_PATH] = {};
  GetSystemDirectoryW(systemRoot, ARRAYSIZE(systemRoot));
  std::wstring bcdedit = systemRoot;
  bcdedit += L"\\bcdedit.exe";

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;

  PROCESS_INFORMATION pi = {};
  std::wstring cmdline = L"\"" + bcdedit + L"\" /enum {current}";
  BOOL created = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(writePipe);
  if (!created) {
    CloseHandle(readPipe);
    return false;
  }

  std::string output;
  char buf[2048];
  DWORD read = 0;
  while (ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) && read > 0) {
    output.append(buf, read);
  }
  CloseHandle(readPipe);
  WaitForSingleObject(pi.hProcess, 10000);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  // Look for a "testsigning  yes" entry (value on the same line).
  const std::string marker = "testsigning";
  const std::string yes = "yes";
  size_t pos = output.find(marker);
  while (pos != std::string::npos) {
    size_t lineEnd = output.find('\n', pos);
    std::string line = output.substr(pos, lineEnd == std::string::npos ? 20 : lineEnd - pos);
    if (line.find(yes) != std::string::npos) return true;
    pos = output.find(marker, pos + marker.size());
  }
  return false;
}
static constexpr UINT DXGI_CAPTURE_TIMEOUT_MS = 3000;
static constexpr wchar_t UNKNOWN_DISPLAY_NAME[] = L"(unknown)";
static constexpr wchar_t VD_ROOT_HARDWARE_ID[] = L"Root\\VIRTUALDISPLAY";
static constexpr wchar_t VD_DEVICE_NAME[] = L"VirtualDisplay Virtual Display Adapter";
static constexpr wchar_t VD_CONFIG_DIR[] = L"C:\\ProgramData\\VirtualDisplay";
static constexpr wchar_t VD_CONFIG_FILE[] = L"C:\\ProgramData\\VirtualDisplay\\monitors.conf";
static constexpr wchar_t VD_LOG_FILE[] = L"C:\\ProgramData\\VirtualDisplay\\VirtualDisplay.log";
static constexpr wchar_t VD_ROOT_CERT_NAME[] = L"VirtualDisplayTestRoot.cer";
static constexpr wchar_t VD_LEAF_CERT_NAME[] = L"VirtualDisplayTestSigning.cer";

static bool g_JsonOutput = false;

/**
 * @brief Release a COM pointer and reset it.
 *
 * @tparam T COM interface type.
 * @param object Pointer slot to release.
 */
template<typename T>
static void ReleaseIfSet(T **object) {
  if (object && *object) {
    (*object)->Release();
    *object = nullptr;
  }
}

/**
 * @brief Convert a UTF-8 string to UTF-16.
 *
 * @param text UTF-8 text.
 * @return Converted UTF-16 text, or an empty string on failure.
 */
static std::wstring Utf8ToWide(const char *text) {
  if (!text) {
    return std::wstring();
  }

  const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  if (count <= 0) {
    return std::wstring();
  }

  std::wstring output(static_cast<size_t>(count - 1), L'\0');
  if (!output.empty()) {
    MultiByteToWideChar(CP_UTF8, 0, text, -1, output.data(), count);
  }
  return output;
}

/**
 * @brief Whether the current process runs with administrator privileges.
 *
 * @return True when the process token contains an enabled administrator group.
 */
static bool IsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }

  TOKEN_ELEVATION elevation = {};
  DWORD size = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

/**
 * @brief Open the user token for a Windows session.
 *
 * The shell process (explorer.exe) token is preferred; WTSQueryUserToken is
 * used as a fallback when no shell is running in the session.
 *
 * @param sessionId Target Windows session ID.
 * @return Primary token handle, or null on failure.
 */
static HANDLE OpenSessionUserToken(DWORD sessionId) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
      if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0) {
        continue;
      }
      DWORD processSession = 0;
      if (!ProcessIdToSessionId(entry.th32ProcessID, &processSession) || processSession != sessionId) {
        continue;
      }
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (!process) {
        continue;
      }
      HANDLE token = nullptr;
      const BOOL opened = OpenProcessToken(
        process,
        TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_IMPERSONATE,
        &token);
      CloseHandle(process);
      if (opened) {
        CloseHandle(snapshot);
        return token;
      }
    }
    CloseHandle(snapshot);
  }

  HANDLE wtsToken = nullptr;
  if (WTSQueryUserToken(sessionId, &wtsToken)) {
    return wtsToken;
  }
  return nullptr;
}

/**
 * @brief Enable a privilege on the current process token when it is available.
 *
 * @param name Privilege name such as `SeAssignPrimaryTokenPrivilege`.
 */
static void EnablePrivilege(const wchar_t *name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return;
  }

  TOKEN_PRIVILEGES privileges = {};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) {
    AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
  }

  CloseHandle(token);
}

/**
 * @brief Relaunch this executable inside a target Windows session.
 *
 * The target process runs as the session user so that GDI, DisplayConfig, and
 * ChangeDisplaySettings calls operate on that session's desktop. The launcher
 * must run as SYSTEM (or hold SeAssignPrimaryTokenPrivilege).
 *
 * @param sessionId Target session ID.
 * @param args Command-line arguments for the in-session copy, without the
 *        `--session` option itself.
 * @return Exit code of the in-session process, or -1 on launch failure.
 */
static int RunInSession(DWORD sessionId, const wchar_t *args) {
  HANDLE sessionToken = OpenSessionUserToken(sessionId);
  if (!sessionToken) {
    std::cerr << "Failed to open session user token for session " << sessionId
              << ". Run the launcher as SYSTEM. error=" << GetLastError() << "\n";
    return -1;
  }

  HANDLE primary = nullptr;
  if (!DuplicateTokenEx(
        sessionToken,
        MAXIMUM_ALLOWED,
        nullptr,
        SecurityImpersonation,
        TokenPrimary,
        &primary)) {
    const DWORD error = GetLastError();
    CloseHandle(sessionToken);
    std::cerr << "DuplicateTokenEx failed. error=" << error << "\n";
    return -1;
  }
  CloseHandle(sessionToken);

  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));

  EnablePrivilege(L"SeAssignPrimaryTokenPrivilege");
  EnablePrivilege(L"SeIncreaseQuotaPrivilege");

  // Build a full command line: quoted application path followed by args.
  // CreateProcess tokenizes lpCommandLine even when lpApplicationName is
  // provided, and a bare "list" would be parsed as the executable name.
  std::wstring commandLine = L"\"";
  commandLine += modulePath;
  commandLine += L"\" ";
  commandLine += args;

  LPVOID environment = nullptr;
  CreateEnvironmentBlock(&environment, primary, FALSE);

  // Redirect the in-session process output to a temp file so the caller can
  // capture it (CreateProcessAsUser does not inherit our console handles).
  wchar_t tempPath[MAX_PATH] = {};
  wchar_t tempFile[MAX_PATH] = {};
  GetTempPathW(ARRAYSIZE(tempPath), tempPath);
  GetTempFileNameW(tempPath, L"vds", 0, tempFile);

  SECURITY_ATTRIBUTES security = {};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE outputFile = CreateFileW(
    tempFile,
    GENERIC_WRITE,
    FILE_SHARE_READ,
    &security,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = outputFile ? outputFile : GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = outputFile ? outputFile : GetStdHandle(STD_ERROR_HANDLE);
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process = {};
  BOOL created = CreateProcessAsUserW(
    primary,
    modulePath,
    commandLine.data(),
    nullptr,
    nullptr,
    TRUE,
    CREATE_UNICODE_ENVIRONMENT,
    environment,
    nullptr,
    &startup,
    &process);
  const DWORD createAsUserError = created ? ERROR_SUCCESS : GetLastError();
  if (!created) {
    created = CreateProcessWithTokenW(
      primary,
      LOGON_WITH_PROFILE,
      modulePath,
      commandLine.data(),
      CREATE_UNICODE_ENVIRONMENT,
      environment,
      nullptr,
      &startup,
      &process);
  }
  const DWORD createWithTokenError = created ? ERROR_SUCCESS : GetLastError();
  if (environment) {
    DestroyEnvironmentBlock(environment);
  }
  CloseHandle(primary);

  if (outputFile) {
    CloseHandle(outputFile);
  }

  if (!created) {
    std::cerr << "CreateProcessAsUserW failed. error=" << createAsUserError << "\n";
    if (createWithTokenError != createAsUserError) {
      std::cerr << "CreateProcessWithTokenW failed. error=" << createWithTokenError << "\n";
    }
    DeleteFileW(tempFile);
    return -1;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  // Echo the in-session output to our own stdout.
  HANDLE readFile = CreateFileW(
    tempFile,
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (readFile != INVALID_HANDLE_VALUE) {
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
      fwrite(buffer, 1, bytesRead, stdout);
    }
    fflush(stdout);
    CloseHandle(readFile);
  }
  DeleteFileW(tempFile);
  return static_cast<int>(exitCode);
}

/**
 * @brief Relaunch this executable elevated and wait for it to finish.
 *
 * @param args Arguments passed to the elevated copy.
 * @return Exit code of the elevated process, or -1 on launch failure.
 */
static int RunElevated(const wchar_t *args) {
  wchar_t modulePath[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath))) {
    std::cerr << "Failed to resolve own module path: " << GetLastError() << "\n";
    return -1;
  }

  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.lpVerb = L"runas";
  info.lpFile = modulePath;
  info.lpParameters = args;
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info) || !info.hProcess) {
    std::cerr << "Elevation failed. Run this command from an elevated prompt. error=" << GetLastError() << "\n";
    return -1;
  }

  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(info.hProcess, &exitCode);
  CloseHandle(info.hProcess);
  return static_cast<int>(exitCode);
}

/**
 * @brief Access masks tried for the control interface.
 */
struct DeviceOpenAttempt {
  DWORD DesiredAccess;  ///< Access mask passed to CreateFileW.
  const wchar_t *Name;  ///< Diagnostic name for the access mask.
};

static constexpr DeviceOpenAttempt DEVICE_OPEN_ATTEMPTS[] = {
  {GENERIC_READ | GENERIC_WRITE, L"rw"},
  {GENERIC_READ, L"read"},
  {0, L"none"},
};

/**
 * @brief Open a Windows device interface path.
 *
 * @param devicePath Device path returned by SetupDi or reconstructed from DeviceClasses.
 * @param openError Receives the system error when CreateFileW fails.
 * @param openAccessName Receives the access mask name used when opening succeeds.
 * @return Open device handle, or INVALID_HANDLE_VALUE when the path cannot be opened.
 */
static HANDLE TryOpenDevicePath(
  const wchar_t *devicePath,
  DWORD *openError,
  const wchar_t **openAccessName = nullptr
) {
  DWORD lastError = ERROR_ACCESS_DENIED;
  for (const auto &attempt : DEVICE_OPEN_ATTEMPTS) {
    HANDLE hDevice = CreateFileW(
      devicePath,
      attempt.DesiredAccess,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr
    );

    if (hDevice != INVALID_HANDLE_VALUE) {
      if (openAccessName) {
        *openAccessName = attempt.Name;
      }
      return hDevice;
    }
    lastError = GetLastError();
  }

  if (openError) {
    *openError = lastError;
  }
  return INVALID_HANDLE_VALUE;
}

/**
 * @brief Enumerate VirtualDisplay IddCx device interfaces and open the first reachable device.
 *
 * @param flags SetupDiGetClassDevs flags controlling which interfaces are enumerated.
 * @return Open device handle, or INVALID_HANDLE_VALUE when no interface can be opened.
 */
static HANDLE OpenDeviceWithFlags(DWORD flags) {
  HDEVINFO devInfo = SetupDiGetClassDevs(
    &GUID_DEVINTERFACE_VIRTUALDISPLAY,
    nullptr,
    nullptr,
    flags
  );

  if (devInfo == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }

  DWORD lastError = ERROR_FILE_NOT_FOUND;
  bool sawInterface = false;
  for (DWORD index = 0;; index++) {
    SP_DEVICE_INTERFACE_DATA ifcData = {};
    ifcData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    BOOL found = SetupDiEnumDeviceInterfaces(
      devInfo,
      nullptr,
      &GUID_DEVINTERFACE_VIRTUALDISPLAY,
      index,
      &ifcData
    );
    if (!found) {
      if (!sawInterface) {
        lastError = GetLastError();
      }
      break;
    }
    sawInterface = true;

    DWORD bufSize = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifcData, nullptr, 0, &bufSize, nullptr);

    std::vector<BYTE> buf(bufSize);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
      reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    BOOL ok = SetupDiGetDeviceInterfaceDetailW(
      devInfo,
      &ifcData,
      detail,
      bufSize,
      nullptr,
      nullptr
    );

    if (!ok) {
      lastError = GetLastError();
      continue;
    }

    DWORD openError = ERROR_SUCCESS;
    HANDLE hDevice = TryOpenDevicePath(detail->DevicePath, &openError);

    if (hDevice != INVALID_HANDLE_VALUE) {
      SetupDiDestroyDeviceInfoList(devInfo);
      return hDevice;
    }
    lastError = openError;
  }

  SetupDiDestroyDeviceInfoList(devInfo);
  SetLastError(lastError);
  return INVALID_HANDLE_VALUE;
}

/**
 * @brief Open the VirtualDisplay IddCx control interface.
 *
 * @return Open device handle, or INVALID_HANDLE_VALUE when no interface can be opened.
 */
static HANDLE OpenDevice() {
  HANDLE device = OpenDeviceWithFlags(DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (device != INVALID_HANDLE_VALUE) {
    return device;
  }
  return OpenDeviceWithFlags(DIGCF_DEVICEINTERFACE);
}

static void PrintUsage() {
  std::cout << "VirtualDisplay control tool\n"
            << "Usage: iddctrl <command> [args] [--json]\n"
            << "\n"
            << "Driver management:\n"
            << "  install [--inf <path>] [--trust-certs]  Install driver package and root device\n"
            << "  uninstall                              Remove root device and driver package\n"
            << "\n"
            << "Virtual monitors:\n"
            << "  add <width> <height> [vsync]           Add a virtual monitor (vsync in mHz, default 60000)\n"
            << "  update <index> <width> <height> [vsync]  Dynamically change monitor mode\n"
            << "  remove <index>                         Remove monitor by index\n"
            << "  clear                                  Remove all monitors\n"
            << "  list                                   List active monitor count\n"
            << "\n"
            << "Configuration persistence:\n"
            << "  save-config [file]                     Save current monitor layout to a config file\n"
            << "  restore [file]                         Restore monitors from a config file\n"
            << "  register-task [off]                    Register/unregister logon auto-restore task\n"
            << "  task-status                            Query auto-restore task state\n"
            << "\n"
            << "Diagnostics:\n"
            << "  caps                                   Show runtime IddCx capabilities\n"
            << "  render-list                            List DXGI graphics adapters\n"
            << "  render-get                             Show current render adapter selection\n"
            << "  render-set auto | id <vendor> <device> [subsys] [rev] [high low]\n"
            << "                                         Set preferred render adapter\n"
            << "  paths                                  Diagnose device interface paths\n"
            << "  displays                               List GDI display devices\n"
            << "  displayconfig                          List active DisplayConfig paths\n"
            << "  advancedcolor [status|on|off] [index]  Query or change HDR state (per monitor)\n"
            << "  primary <index>                        Make the virtual monitor the primary display\n"
            << "  physical-primary                       Restore the physical display as primary\n"
            << "  layout <index:x,y> [<index:x,y>...]    Set monitor desktop positions\n"
            << "  dxgicap [display-name]                 Capture one DXGI duplication frame\n"
            << "\n"
            << "Global option:\n"
            << "  --json                                 Machine-readable JSON output for GUI integration\n"
            << "\n"
            << "Cross-session:\n"
            << "  --session <id> <command> [args...]     Run a command in the target session (requires SYSTEM)\n"
            << "  run <exe> [args...]                    Launch an arbitrary program (use with --session)\n"
            << "  run-here <exe> [args...]               Launch a program on the monitor under the cursor\n"
            << "  run-display <index> <exe> [args...]    Launch a program on a specific virtual display\n";
}

/**
 * @brief Write a JSON string literal with escaping.
 *
 * @param text Text to write.
 */
static void JsonWriteString(const char *text) {
  std::cout << '"';
  for (const char *p = text; *p; p++) {
    switch (*p) {
      case '"': std::cout << "\\\""; break;
      case '\\': std::cout << "\\\\"; break;
      case '\n': std::cout << "\\n"; break;
      case '\r': std::cout << "\\r"; break;
      case '\t': std::cout << "\\t"; break;
      default: std::cout << *p; break;
    }
  }
  std::cout << '"';
}

/**
 * @brief Write a JSON string literal with escaping (wide-character variant).
 *
 * @param text Text to write.
 */
static void JsonWriteStringW(const wchar_t *text) {
  std::wcout << L'"';
  for (const wchar_t *p = text; *p; p++) {
    switch (*p) {
      case L'"': std::wcout << L"\\\""; break;
      case L'\\': std::wcout << L"\\\\"; break;
      case L'\n': std::wcout << L"\\n"; break;
      case L'\r': std::wcout << L"\\r"; break;
      case L'\t': std::wcout << L"\\t"; break;
      default: std::wcout << *p; break;
    }
  }
  std::wcout << L'"';
}

/**
 * @brief Return whether a GDI display device belongs to the VirtualDisplay adapter.
 *
 * @param device Display device descriptor returned by `EnumDisplayDevicesW`.
 * @return True when the device string or PnP ID matches the VirtualDisplay adapter.
 */
static bool IsVirtualDisplayDevice(const DISPLAY_DEVICEW &device) {
  return wcsstr(device.DeviceID, L"ROOT\\VIRTUALDISPLAY") ||
         wcsstr(device.DeviceString, L"VirtualDisplay Virtual Display");
}

/**
 * @brief Count active VirtualDisplay adapters in the GDI enumeration.
 *
 * @return Number of active VirtualDisplay display devices.
 */
static UINT32 CountActiveVirtualDisplays() {
  UINT32 count = 0;
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }
    if (IsVirtualDisplayDevice(adapter) && (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
      count++;
    }
  }
  return count;
}

/**
 * @brief Wait until the requested monitor index appears as an active GDI display.
 *
 * Newly created virtual displays take a moment to be registered by the
 * display stack. Applying a desktop mode before the target is active can
 * fail or target the wrong monitor, so callers wait for the target to settle.
 *
 * @param monitorIndex One-based monitor index to wait for.
 * @param timeoutMs Maximum wait time.
 * @return True when the monitor became active within the timeout.
 */
static bool WaitForMonitorActive(UINT32 monitorIndex, DWORD timeoutMs) {
  const DWORD deadline = GetTickCount() + timeoutMs;
  while (GetTickCount() < deadline) {
    if (CountActiveVirtualDisplays() >= monitorIndex) {
      return true;
    }
    Sleep(200);
  }
  return CountActiveVirtualDisplays() >= monitorIndex;
}

/**
 * @brief Find the active VirtualDisplay display device name for capture probes.
 *
 * @return GDI display name such as `\\.\DISPLAY13`, or an empty string.
 */
static std::wstring FindActiveVirtualDisplayName() {
  std::wstring fallback;
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }

    if (!IsVirtualDisplayDevice(adapter)) {
      continue;
    }
    if (fallback.empty()) {
      fallback = adapter.DeviceName;
    }
    if ((adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0) {
      return adapter.DeviceName;
    }
  }
  return fallback;
}

/**
 * @brief Find the active DisplayConfig path for the VirtualDisplay adapter.
 *
 * @param selectedPath Receives the selected active display path.
 * @return True when an active VirtualDisplay path was found.
 */
static bool FindActiveVirtualDisplayConfigPath(DISPLAYCONFIG_PATH_INFO *selectedPath, UINT32 wantIndex = 1) {
  if (!selectedPath) {
    return false;
  }

  for (UINT attempt = 0; attempt < 3; attempt++) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS) {
      return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    result = QueryDisplayConfig(
      QDC_ONLY_ACTIVE_PATHS,
      &pathCount,
      paths.data(),
      &modeCount,
      modes.data(),
      nullptr
    );
    if (result == ERROR_INSUFFICIENT_BUFFER) {
      continue;
    }
    if (result != ERROR_SUCCESS) {
      return false;
    }

    UINT32 virtIndex = 0;
    for (UINT32 index = 0; index < pathCount; index++) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
      sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      sourceName.header.size = sizeof(sourceName);
      sourceName.header.adapterId = paths[index].sourceInfo.adapterId;
      sourceName.header.id = paths[index].sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
        continue;
      }

      // Is this source one of our VirtualDisplay adapters (by GDI name)?
      bool isVd = false;
      for (DWORD di = 0;; di++) {
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, di, &adapter, 0)) break;
        if (!IsVirtualDisplayDevice(adapter)) continue;
        if (_wcsicmp(adapter.DeviceName, sourceName.viewGdiDeviceName) == 0) {
          isVd = true;
          break;
        }
      }
      if (!isVd) continue;

      virtIndex++;
      if (virtIndex == wantIndex) {
        *selectedPath = paths[index];
        return true;
      }
    }
  }

  return false;
}

/**
 * @brief Create a D3D11 device on a DXGI adapter.
 *
 * @param adapter Adapter backing the selected output.
 * @param device Receives a D3D11 device.
 * @param context Receives the immediate context.
 * @return Direct3D result code.
 */
static HRESULT CreateD3D11DeviceForAdapter(
  IDXGIAdapter1 *adapter,
  ID3D11Device **device,
  ID3D11DeviceContext **context
) {
  static const D3D_FEATURE_LEVEL featureLevels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
  };

  D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_11_0;
  return D3D11CreateDevice(
    adapter,
    D3D_DRIVER_TYPE_UNKNOWN,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    featureLevels,
    ARRAYSIZE(featureLevels),
    D3D11_SDK_VERSION,
    device,
    &selectedLevel,
    context
  );
}

/**
 * @brief Compute a simple FNV-1a hash over a mapped texture.
 *
 * @param mapped Mapped staging texture.
 * @param height Texture height.
 * @return Non-cryptographic content hash.
 */
static uint64_t HashMappedTexture(const D3D11_MAPPED_SUBRESOURCE &mapped, UINT height) {
  uint64_t hash = 1469598103934665603ull;
  const auto *base = static_cast<const uint8_t *>(mapped.pData);
  for (UINT y = 0; y < height; y++) {
    const uint8_t *row = base + static_cast<size_t>(y) * mapped.RowPitch;
    for (UINT x = 0; x < mapped.RowPitch; x++) {
      hash ^= row[x];
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

/**
 * @brief Count non-zero bytes in a mapped texture.
 *
 * @param mapped Mapped staging texture.
 * @param height Texture height.
 * @return Number of non-zero bytes.
 */
static uint64_t CountNonZeroBytes(const D3D11_MAPPED_SUBRESOURCE &mapped, UINT height) {
  uint64_t count = 0;
  const auto *base = static_cast<const uint8_t *>(mapped.pData);
  for (UINT y = 0; y < height; y++) {
    const uint8_t *row = base + static_cast<size_t>(y) * mapped.RowPitch;
    for (UINT x = 0; x < mapped.RowPitch; x++) {
      if (row[x] != 0) {
        count++;
      }
    }
  }
  return count;
}

/**
 * @brief Nudge the desktop so output duplication has a fresh frame to return.
 */
static void TriggerSmallDesktopUpdate() {
  POINT cursor = {};
  if (GetCursorPos(&cursor)) {
    SetCursorPos(cursor.x + 1, cursor.y);
    SetCursorPos(cursor.x, cursor.y);
  }
}

/**
 * @brief Duplicate one DXGI output frame and print diagnostics.
 *
 * @param output DXGI output to duplicate.
 * @param device D3D11 device created on the output adapter.
 * @param context D3D11 immediate context.
 * @return Zero when a frame was captured.
 */
static int CaptureOneDxgiFrame(
  IDXGIOutput1 *output,
  ID3D11Device *device,
  ID3D11DeviceContext *context
) {
  IDXGIOutputDuplication *duplication = nullptr;
  HRESULT hr = output->DuplicateOutput(device, &duplication);
  if (FAILED(hr)) {
    std::cout << "DuplicateOutput failed hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    return 1;
  }

  TriggerSmallDesktopUpdate();

  DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
  IDXGIResource *resource = nullptr;
  hr = duplication->AcquireNextFrame(DXGI_CAPTURE_TIMEOUT_MS, &frameInfo, &resource);
  if (FAILED(hr)) {
    std::cout << "AcquireNextFrame failed hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    ReleaseIfSet(&duplication);
    return 1;
  }

  ID3D11Texture2D *texture = nullptr;
  hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
  ReleaseIfSet(&resource);
  if (FAILED(hr)) {
    std::cout << "Frame QueryInterface(ID3D11Texture2D) failed hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    duplication->ReleaseFrame();
    ReleaseIfSet(&duplication);
    return 1;
  }

  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  D3D11_TEXTURE2D_DESC stagingDesc = desc;
  stagingDesc.Usage = D3D11_USAGE_STAGING;
  stagingDesc.BindFlags = 0;
  stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  stagingDesc.MiscFlags = 0;

  ID3D11Texture2D *staging = nullptr;
  hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
  if (FAILED(hr)) {
    std::cout << "Create staging texture failed hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    ReleaseIfSet(&texture);
    duplication->ReleaseFrame();
    ReleaseIfSet(&duplication);
    return 1;
  }

  context->CopyResource(staging, texture);
  ReleaseIfSet(&texture);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    std::cout << "Map staging texture failed hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    ReleaseIfSet(&staging);
    duplication->ReleaseFrame();
    ReleaseIfSet(&duplication);
    return 1;
  }

  const uint64_t hash = HashMappedTexture(mapped, desc.Height);
  const uint64_t nonZero = CountNonZeroBytes(mapped, desc.Height);
  context->Unmap(staging, 0);
  ReleaseIfSet(&staging);

  std::cout << "dxgi_frame width=" << desc.Width
            << " height=" << desc.Height
            << " format=" << desc.Format
            << " accumulated=" << frameInfo.AccumulatedFrames
            << " row_pitch=" << mapped.RowPitch
            << " nonzero_bytes=" << nonZero
            << " hash=0x" << std::hex << hash << std::dec
            << "\n";

  duplication->ReleaseFrame();
  ReleaseIfSet(&duplication);
  return 0;
}

/**
 * @brief Return whether a GDI mode matches a requested monitor descriptor.
 *
 * @param mode Current display mode returned by Windows.
 * @param desc Requested monitor descriptor.
 * @return True when resolution and refresh match.
 */
static bool ModeMatches(const DEVMODEW &mode, const MonitorDesc &desc) {
  return mode.dmPelsWidth == desc.Width &&
         mode.dmPelsHeight == desc.Height &&
         mode.dmDisplayFrequency == desc.VSync / VSYNC_MHZ_DENOMINATOR;
}

/**
 * @brief Print a GDI display mode in a compact diagnostic format.
 *
 * @param prefix Text written before the mode.
 * @param mode Display mode to print.
 */
static void PrintDisplayMode(const wchar_t *prefix, const DEVMODEW &mode) {
  std::wcout << prefix
             << mode.dmPelsWidth << L"x" << mode.dmPelsHeight
             << L"@" << mode.dmDisplayFrequency
             << L" bpp=" << mode.dmBitsPerPel
             << L" pos=" << mode.dmPosition.x << L"," << mode.dmPosition.y
             << L"\n";
}

/**
 * @brief Find the X coordinate where a newly attached display should be placed.
 *
 * @return Right edge of the current active desktop layout.
 */
static LONG FindAttachPositionX() {
  LONG right = 0;
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }

    if ((adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) {
      continue;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
      continue;
    }

    const LONG adapterRight = mode.dmPosition.x + static_cast<LONG>(mode.dmPelsWidth);
    if (adapterRight > right) {
      right = adapterRight;
    }
  }
  return right;
}

/**
 * @brief Print GDI display devices that may be used for desktop mode changes.
 *
 * @return Zero when enumeration completed.
 */
static int CmdDisplays() {
  bool firstJson = true;
  if (g_JsonOutput) {
    std::cout << "{\"displays\":[";
  }
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }

    if (g_JsonOutput) {
      if (!firstJson) {
        std::cout << ",";
      }
      firstJson = false;
      std::wcout << L"{\"device\":";
      JsonWriteString("adapter");
      std::wcout << L",\"name\":";
      JsonWriteString("adapter");
      std::wcout << L",\"device_name\":";
      JsonWriteString("adapter");
      std::wcout << L",\"value\":";
      {
        std::wstring value = adapter.DeviceName;
        value += L"|";
        value += adapter.DeviceString;
        value += L"|flags=0x";
        wchar_t flagsBuf[16] = {};
        swprintf_s(flagsBuf, L"%X", adapter.StateFlags);
        value += flagsBuf;
        value += L"|id=";
        value += adapter.DeviceID;
        JsonWriteStringW(value.c_str());
      }
      std::wcout << L",\"current\":";
      if (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) {
        DEVMODEW cur = {};
        cur.dmSize = sizeof(cur);
        if (EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &cur, 0)) {
          std::cout << "{\"w\":" << cur.dmPelsWidth
                    << ",\"h\":" << cur.dmPelsHeight
                    << ",\"rate\":" << cur.dmDisplayFrequency
                    << ",\"x\":" << cur.dmPosition.x
                    << ",\"y\":" << cur.dmPosition.y << "}";
        } else {
          std::cout << "null";
        }
      } else {
        std::cout << "null";
      }
      std::cout << ",\"virtual\":" << (IsVirtualDisplayDevice(adapter) ? "true" : "false")
                << ",\"primary\":" << ((adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? "true" : "false");
      std::cout << ",\"modes\":[";
      if (IsVirtualDisplayDevice(adapter)) {
        bool firstMode = true;
        for (DWORD modeIndex = 0; modeIndex < 32; modeIndex++) {
          DEVMODEW mode = {};
          mode.dmSize = sizeof(mode);
          if (!EnumDisplaySettingsExW(adapter.DeviceName, modeIndex, &mode, 0)) {
            break;
          }
          if (!firstMode) {
            std::cout << ",";
          }
          firstMode = false;
          std::cout << "{\"w\":" << mode.dmPelsWidth
                    << ",\"h\":" << mode.dmPelsHeight
                    << ",\"rate\":" << mode.dmDisplayFrequency << "}";
        }
      }
      std::cout << "]}";
      continue;
    }

    std::wcout << L"adapter " << index << L": "
               << adapter.DeviceName << L" | "
               << adapter.DeviceString << L" | flags=0x"
               << std::hex << adapter.StateFlags << std::dec
               << L" | id=" << adapter.DeviceID << L"\n";

    if (!IsVirtualDisplayDevice(adapter)) {
      continue;
    }

    for (DWORD modeIndex = 0; modeIndex < 32; modeIndex++) {
      DEVMODEW mode = {};
      mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsExW(adapter.DeviceName, modeIndex, &mode, 0)) {
        if (modeIndex == 0) {
          std::wcout << L"  no enumerated modes\n";
        }
        break;
      }

      std::wcout << L"  mode " << modeIndex << L": "
                 << mode.dmPelsWidth << L"x" << mode.dmPelsHeight
                 << L"@" << mode.dmDisplayFrequency
                 << L" bpp=" << mode.dmBitsPerPel
                 << L" pos=" << mode.dmPosition.x << L"," << mode.dmPosition.y
                 << L"\n";
    }
  }
  if (g_JsonOutput) {
    std::cout << "]}\n";
  }
  return 0;
}

/**
 * @brief Print active DisplayConfig paths and source/target modes.
 *
 * @return Zero when DisplayConfig data was read.
 */
static int CmdDisplayConfig() {
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
  if (result != ERROR_SUCCESS) {
    std::cerr << "GetDisplayConfigBufferSizes failed: " << result << "\n";
    return 1;
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  result = QueryDisplayConfig(
    QDC_ONLY_ACTIVE_PATHS,
    &pathCount,
    paths.data(),
    &modeCount,
    modes.data(),
    nullptr
  );
  if (result != ERROR_SUCCESS) {
    std::cerr << "QueryDisplayConfig failed: " << result << "\n";
    return 1;
  }

  std::wcout << L"active_paths=" << pathCount << L" modes=" << modeCount << L"\n";
  for (UINT32 index = 0; index < pathCount; index++) {
    const DISPLAYCONFIG_PATH_INFO &path = paths[index];
    std::wcout << L"path " << index
               << L": flags=0x" << std::hex << path.flags << std::dec
               << L" source=" << path.sourceInfo.id
               << L" target=" << path.targetInfo.id
               << L" adapter=" << path.sourceInfo.adapterId.HighPart << L":" << path.sourceInfo.adapterId.LowPart
               << L" available=" << (path.targetInfo.targetAvailable ? L"yes" : L"no");

    if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && path.sourceInfo.modeInfoIdx < modeCount) {
      const DISPLAYCONFIG_MODE_INFO &mode = modes[path.sourceInfo.modeInfoIdx];
      if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        std::wcout << L" source_mode="
                   << mode.sourceMode.width << L"x" << mode.sourceMode.height
                   << L" pos=" << mode.sourceMode.position.x << L"," << mode.sourceMode.position.y;
      }
    }

    if (path.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && path.targetInfo.modeInfoIdx < modeCount) {
      const DISPLAYCONFIG_MODE_INFO &mode = modes[path.targetInfo.modeInfoIdx];
      if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
        const DISPLAYCONFIG_VIDEO_SIGNAL_INFO &signal = mode.targetMode.targetVideoSignalInfo;
        const UINT32 refresh = signal.vSyncFreq.Denominator ?
                                 signal.vSyncFreq.Numerator / signal.vSyncFreq.Denominator :
                                 0;
        std::wcout << L" target_mode="
                   << signal.activeSize.cx << L"x" << signal.activeSize.cy
                   << L"@" << refresh;
      }
    }
    std::wcout << L"\n";
  }

  return 0;
}

/**
 * @brief Apply a monitor mode to the current desktop path using user-mode display APIs.
 *
 * The target monitor is matched by the position of the active VirtualDisplay
 * device in the GDI enumeration. Windows enumerates display devices in
 * connector order, which matches the driver slot order (verified: GDI order
 * 113/114/115 maps to target ids 256/257/258). Stale (inactive) virtual
 * displays are skipped so they cannot skew the count.
 *
 * @param desc Monitor descriptor containing the requested width, height, and refresh rate.
 * @return True when `ChangeDisplaySettingsExW` accepted the mode.
 */
static bool ApplyDesktopMode(const MonitorDesc &desc) {
  UINT32 virtualIndex = 0;
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }

    if (!IsVirtualDisplayDevice(adapter) || !(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
      continue;
    }
    virtualIndex++;
    if (desc.MonitorIndex != 0 && virtualIndex != desc.MonitorIndex) {
      continue;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    const bool active = (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;
    const bool hasCurrentMode = EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE;
    if (!hasCurrentMode) {
      mode = {};
      mode.dmSize = sizeof(mode);
      mode.dmPosition.x = FindAttachPositionX();
      mode.dmPosition.y = 0;
    }

    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    mode.dmPelsWidth = desc.Width;
    mode.dmPelsHeight = desc.Height;
    mode.dmBitsPerPel = 32;
    mode.dmDisplayFrequency = desc.VSync / VSYNC_MHZ_DENOMINATOR;

    const LONG result = ChangeDisplaySettingsExW(
      adapter.DeviceName,
      &mode,
      nullptr,
      active ? CDS_UPDATEREGISTRY : (CDS_UPDATEREGISTRY | CDS_NORESET),
      nullptr
    );
    if (result != DISP_CHANGE_SUCCESSFUL) {
      std::wcerr << L"ChangeDisplaySettingsEx failed for " << adapter.DeviceName
                 << L". result=" << result << L"\n";
      continue;
    }

    const LONG applyResult = active ?
                               DISP_CHANGE_SUCCESSFUL :
                               ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (applyResult == DISP_CHANGE_SUCCESSFUL) {
      DEVMODEW currentMode = {};
      currentMode.dmSize = sizeof(currentMode);
      const bool hasFinalMode = EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &currentMode, 0) != FALSE;
      if (hasFinalMode) {
        PrintDisplayMode(L"Final desktop mode: ", currentMode);
      } else {
        std::wcerr << L"Failed to read final desktop mode for " << adapter.DeviceName
                   << L". error=" << GetLastError() << L"\n";
      }

      if (hasFinalMode && !ModeMatches(currentMode, desc)) {
        std::wcerr << L"Desktop mode mismatch on " << adapter.DeviceName
                   << L". requested=" << desc.Width << L"x" << desc.Height
                   << L"@" << desc.VSync / VSYNC_MHZ_DENOMINATOR << L"\n";
        return false;
      }

      std::wcout << L"Desktop mode applied on " << adapter.DeviceName
                 << L" active=" << (active ? L"yes" : L"no") << L"\n";
      return true;
    }

    std::wcerr << L"Desktop apply failed after staging " << adapter.DeviceName
               << L". result=" << applyResult << L"\n";
  }

  return false;
}

/**
 * @brief Enumerate active VirtualDisplay adapters in GDI order.
 *
 * The k-th active VirtualDisplay adapter corresponds to the k-th one-based
 * monitor index used by the driver (verified: GDI order == slot order).
 *
 * @param names Receives device names of VirtualDisplay adapters, indexed by slot-1.
 * @param positions Receives current (x, y) desktop positions, indexed by slot-1.
 * @param widths Receives current widths, indexed by slot-1.
 * @param heights Receives current heights, indexed by slot-1.
 * @return Number of active VirtualDisplay adapters found.
 */
static UINT32 EnumerateVirtualDisplays(
  std::vector<std::wstring> *names,
  std::vector<POINT> *positions,
  std::vector<LONG> *widths,
  std::vector<LONG> *heights
) {
  UINT32 count = 0;
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }
    if (!IsVirtualDisplayDevice(adapter) || !(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
      continue;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
      continue;
    }

    count++;
    if (names) names->push_back(adapter.DeviceName);
    if (positions) positions->push_back({ mode.dmPosition.x, mode.dmPosition.y });
    if (widths) widths->push_back(static_cast<LONG>(mode.dmPelsWidth));
    if (heights) heights->push_back(static_cast<LONG>(mode.dmPelsHeight));
  }
  return count;
}

/**
 * @brief Enumerate ALL active displays (physical and virtual) in GDI order.
 *
 * Unlike EnumerateVirtualDisplays this includes physical monitors, which is
 * required when moving the primary: the physical display must be shifted out
 * of (0,0) for a virtual display to become primary.
 *
 * @param names Receives device names, indexed by list position.
 * @param positions Receives current (x, y) desktop positions.
 * @param widths Receives current widths.
 * @param heights Receives current heights.
 * @param virtualFlags Receives true for VirtualDisplay adapters.
 * @return Number of active displays found.
 */
static UINT32 EnumerateAllDisplays(
  std::vector<std::wstring> *names,
  std::vector<POINT> *positions,
  std::vector<LONG> *widths,
  std::vector<LONG> *heights,
  std::vector<bool> *virtualFlags
) {
  UINT32 count = 0;
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }
    if (!(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
      continue;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
      continue;
    }

    count++;
    if (names) names->push_back(adapter.DeviceName);
    if (positions) positions->push_back({ mode.dmPosition.x, mode.dmPosition.y });
    if (widths) widths->push_back(static_cast<LONG>(mode.dmPelsWidth));
    if (heights) heights->push_back(static_cast<LONG>(mode.dmPelsHeight));
    if (virtualFlags) virtualFlags->push_back(IsVirtualDisplayDevice(adapter));
  }
  return count;
}

/**
 * @brief Apply a desktop layout by staging every display's position then
 *        committing with a single ChangeDisplaySettingsEx(nullptr).
 *
 * Verified working for primary moves on a Hyper-V VM (moving a non-primary
 * virtual display into (0,0) makes it primary and Windows shifts the old
 * primary away). On some physical GPUs (e.g. Intel internal panels) moving a
 * display onto an occupied origin is rejected — that is a system limitation,
 * not a driver bug. SetDisplayConfig was also tried (Windows settings API)
 * but returns ERROR_INVALID_PARAMETER in these sessions.
 *
 * @param deviceNames Display device names.
 * @param positions Desired (x, y) for each display.
 * @param widths Display widths (used to preserve mode geometry).
 * @param heights Display heights.
 * @return True when the layout commit succeeded.
 */
static bool ApplyDesktopLayout(
  const std::vector<std::wstring> &deviceNames,
  const std::vector<POINT> &positions,
  const std::vector<LONG> &widths,
  const std::vector<LONG> &heights
) {
  if (deviceNames.empty()) {
    return false;
  }

  bool anyStaged = false;
  for (size_t i = 0; i < deviceNames.size(); i++) {
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(deviceNames[i].c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
      mode = {};
      mode.dmSize = sizeof(mode);
    }
    mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    mode.dmPosition.x = positions[i].x;
    mode.dmPosition.y = positions[i].y;
    mode.dmPelsWidth = static_cast<DWORD>(widths[i]);
    mode.dmPelsHeight = static_cast<DWORD>(heights[i]);
    mode.dmBitsPerPel = 32;

    const LONG result = ChangeDisplaySettingsExW(
      deviceNames[i].c_str(),
      &mode,
      nullptr,
      CDS_UPDATEREGISTRY | CDS_NORESET,
      nullptr
    );
    if (result != DISP_CHANGE_SUCCESSFUL) {
      std::wcerr << L"Layout stage failed for " << deviceNames[i]
                 << L". result=" << result << L"\n";
      continue;
    }
    anyStaged = true;
  }

  if (!anyStaged) {
    return false;
  }

  const LONG applyResult = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
  return applyResult == DISP_CHANGE_SUCCESSFUL;
}

/**
 * @brief Move the display currently occupying a position away.
 *
 * SetDisplayConfig rejects layouts where two displays share the same origin.
 * When a target position is occupied by another display, push that occupant
 * to the right of the rightmost display so the layout can be applied.
 *
 * @param posX Target X position that must be vacated.
 * @param posY Target Y position that must be vacated.
 * @param excludeName Device name to skip (the mover itself).
 * @return True when the position was vacated (or was already free).
 */
static bool VacatePosition(LONG posX, LONG posY, const std::wstring &excludeName) {
  std::vector<std::wstring> names;
  std::vector<POINT> positions;
  std::vector<LONG> widths;
  std::vector<LONG> heights;
  const UINT32 count = EnumerateAllDisplays(&names, &positions, &widths, &heights, nullptr);

  UINT32 occupant = count;
  for (UINT32 i = 0; i < count; i++) {
    if (!excludeName.empty() && _wcsicmp(names[i].c_str(), excludeName.c_str()) == 0) {
      continue;
    }
    if (positions[i].x == posX && positions[i].y == posY) {
      occupant = i;
      break;
    }
  }
  if (occupant >= count) {
    return true;  // position is free
  }
  if (count < 2) {
    return true;  // nothing to move it to
  }

  LONG cursorX = 0;
  bool cursorSet = false;
  for (UINT32 i = 0; i < count; i++) {
    if (i == occupant) continue;
    const LONG right = positions[i].x + widths[i];
    if (!cursorSet || right > cursorX) {
      cursorX = right;
      cursorSet = true;
    }
  }
  const POINT newPos = { cursorX, positions[occupant].y };

  std::vector<std::wstring> vNames;
  std::vector<POINT> vPositions;
  std::vector<LONG> vWidths;
  std::vector<LONG> vHeights;
  vNames.push_back(names[occupant]);
  vPositions.push_back(newPos);
  vWidths.push_back(widths[occupant]);
  vHeights.push_back(heights[occupant]);
  return ApplyDesktopLayout(vNames, vPositions, vWidths, vHeights);
}

/**
 * @brief Move the display currently at (0,0) away so another can become primary.
 *
 * @return True when the primary was vacated (or there was nothing to vacate).
 */
static bool VacatePrimary() {
  return VacatePosition(0, 0, std::wstring());
}

/**
 * @brief Set one virtual display as the primary by moving it to (0,0).
 *
 * @param index One-based monitor index to make primary.
 * @return Zero on success.
 */
static int CmdPrimary(UINT32 index) {
  std::vector<std::wstring> names;
  std::vector<POINT> positions;
  std::vector<LONG> widths;
  std::vector<LONG> heights;
  std::vector<bool> virtualFlags;
  // Enumerate ALL displays (physical + virtual) so the physical monitor is
  // shifted out of (0,0) too; otherwise Windows never switches the primary.
  const UINT32 count = EnumerateAllDisplays(&names, &positions, &widths, &heights, &virtualFlags);

  // The requested index refers to the virtual display slot (1-based). Walk the
  // list to find that slot's display.
  UINT32 virtSeen = 0;
  UINT32 targetPos = count;  // position in the arrays
  for (UINT32 i = 0; i < count; i++) {
    if (!virtualFlags[i]) continue;
    virtSeen++;
    if (virtSeen == index) {
      targetPos = i;
      break;
    }
  }
  if (targetPos >= count) {
    std::cerr << "Monitor " << index << " not found among active virtual displays\n";
    return 1;
  }

  const POINT origin = positions[targetPos];

  // Move ONLY the target virtual display to (0,0). ChangeDisplaySettingsEx
  // can move this (it is non-primary), and Windows auto-shifts the current
  // primary away, reassigning the primary to the target. Shifting every
  // display (the old approach) fails because moving the primary to negative
  // coordinates is rejected and stale displays can be enumerated.
  if (origin.x == 0 && origin.y == 0) {
    if (g_JsonOutput) {
      std::cout << "{\"primary\":" << index << ",\"already\":true}\n";
    } else {
      std::cout << "Monitor " << index << " is already the primary display\n";
    }
    return 0;
  }

  std::vector<std::wstring> vNames;
  std::vector<POINT> vPositions;
  std::vector<LONG> vWidths;
  std::vector<LONG> vHeights;
  vNames.push_back(names[targetPos]);
  vPositions.push_back({ 0, 0 });
  vWidths.push_back(widths[targetPos]);
  vHeights.push_back(heights[targetPos]);

  if (g_JsonOutput) {
    std::cout << "{\"primary\":" << index << ",\"move\":{\"x\":0,\"y\":0}}\n";
  }

  if (!ApplyDesktopLayout(vNames, vPositions, vWidths, vHeights)) {
    std::cerr << "Failed to apply primary layout\n";
    return 1;
  }

  if (!g_JsonOutput) {
    std::cout << "Monitor " << index << " is now the primary display\n";
  }
  return 0;
}

/**
 * @brief Make the first physical display the primary again.
 *
 * Shifts every VIRTUAL display to the right of the physical monitor. The
 * physical display itself is left untouched: on most systems (and Hyper-V)
 * ChangeDisplaySettingsEx on the physical display fails, but Windows
 * automatically reclaims (0,0) for the physical monitor once the virtual
 * displays vacate it.
 *
 * @return Zero on success.
 */
static int CmdPhysicalPrimary() {
  std::vector<std::wstring> names;
  std::vector<POINT> positions;
  std::vector<LONG> widths;
  std::vector<LONG> heights;
  std::vector<bool> virtualFlags;
  const UINT32 count = EnumerateAllDisplays(&names, &positions, &widths, &heights, &virtualFlags);

  UINT32 physPos = count;
  for (UINT32 i = 0; i < count; i++) {
    if (!virtualFlags[i]) {
      physPos = i;
      break;
    }
  }
  if (physPos >= count) {
    std::cerr << "No physical display found\n";
    return 1;
  }

  // Move every virtual display to the right of the physical one. If a virtual
  // display currently owns (0,0) this still works on most systems because
  // Windows reclaims the primary when the occupant leaves (verified on VM
  // where a virtual primary was relocated). If the virtual is the primary and
  // the move fails (some Intel panels), the caller is told to use the Windows
  // display settings UI, which uses SetDisplayConfig internally.
  LONG cursorX = positions[physPos].x + widths[physPos];
  const LONG baseY = positions[physPos].y;
  std::vector<std::wstring> virtNames;
  std::vector<POINT> virtPositions;
  std::vector<LONG> virtWidths;
  std::vector<LONG> virtHeights;
  for (UINT32 i = 0; i < count; i++) {
    if (!virtualFlags[i]) continue;
    virtNames.push_back(names[i]);
    virtPositions.push_back({ cursorX, baseY });
    virtWidths.push_back(widths[i]);
    virtHeights.push_back(heights[i]);
    cursorX += widths[i];
  }

  if (g_JsonOutput) {
    std::cout << "{\"primary\":\"physical\",\"virtual_layout\":[";
    bool first = true;
    for (UINT32 i = 0; i < virtPositions.size(); i++) {
      if (!first) std::cout << ",";
      first = false;
      std::cout << "{\"index\":" << (i + 1)
                << ",\"x\":" << virtPositions[i].x
                << ",\"y\":" << virtPositions[i].y << "}";
    }
    std::cout << "]}\n";
  }

  if (!ApplyDesktopLayout(virtNames, virtPositions, virtWidths, virtHeights)) {
    std::cerr << "Failed to restore physical primary. If a virtual display is "
                 "currently the primary, Windows may refuse to move it; use the "
                 "Windows display settings UI instead.\n";
    return 1;
  }

  if (!g_JsonOutput) {
    std::cout << "Physical display is now the primary again\n";
  }
  return 0;
}

/**
 * @brief Set desktop positions for virtual displays.
 *
 * @param layout Pairs of "index:x,y" arguments.
 * @return Zero on success.
 */
static int CmdLayout(const std::vector<std::string> &layout) {
  std::vector<std::wstring> names;
  std::vector<POINT> positions;
  std::vector<LONG> widths;
  std::vector<LONG> heights;
  const UINT32 count = EnumerateVirtualDisplays(&names, &positions, &widths, &heights);

  std::vector<POINT> newPositions = positions;
  for (const std::string &item : layout) {
    std::string itemCopy = item;
    const size_t colon = itemCopy.find(':');
    if (colon == std::string::npos) {
      std::cerr << "Invalid layout item: " << item << " (expected index:x,y)\n";
      return 1;
    }
    const UINT32 index = static_cast<UINT32>(std::stoul(itemCopy.substr(0, colon)));
    if (index == 0 || index > count) {
      std::cerr << "Monitor " << index << " not found\n";
      return 1;
    }

    const std::string coord = itemCopy.substr(colon + 1);
    const size_t comma = coord.find(',');
    if (comma == std::string::npos) {
      std::cerr << "Invalid layout item: " << item << " (expected index:x,y)\n";
      return 1;
    }
    newPositions[index - 1].x = std::stol(coord.substr(0, comma));
    newPositions[index - 1].y = std::stol(coord.substr(comma + 1));
  }

  if (g_JsonOutput) {
    std::cout << "{\"layout\":[";
    for (UINT32 i = 0; i < count; i++) {
      if (i > 0) std::cout << ",";
      std::cout << "{\"index\":" << (i + 1)
                << ",\"x\":" << newPositions[i].x
                << ",\"y\":" << newPositions[i].y << "}";
    }
    std::cout << "]}\n";
  }

  // If any target monitor currently occupies (0,0) (it is the primary) and is
  // being moved, ChangeDisplaySettingsEx cannot move it. Vacate (0,0) first
  // by moving the current primary away; Windows reassigns the primary.
  for (size_t i = 0; i < count; i++) {
    if (positions[i].x == 0 && positions[i].y == 0 &&
        (newPositions[i].x != 0 || newPositions[i].y != 0)) {
      if (!VacatePosition(0, 0, names[i])) {
        std::cerr << "Failed to vacate primary for monitor " << (i + 1) << "\n";
        return 1;
      }
      break;
    }
  }

  if (!ApplyDesktopLayout(names, newPositions, widths, heights)) {
    std::cerr << "Failed to apply layout\n";
    return 1;
  }

  if (!g_JsonOutput) {
    std::cout << "Layout applied\n";
  }
  return 0;
}

/**
 * @brief Validate a monitor geometry request against basic sanity bounds.
 *
 * The virtual display has no EDID, so the advertised mode table is the only
 * limit and custom resolutions/refresh rates are forwarded to the display
 * stack, which decides whether the render GPU can handle them. Only sanity
 * bounds (nonzero, non-absurd) are enforced here.
 *
 * @param width Requested width.
 * @param height Requested height.
 * @param vsync Requested refresh in millihertz.
 * @return True when the request is sane.
 */
static bool ValidateMonitorGeometry(UINT32 width, UINT32 height, UINT32 vsync) {
  if (width < 320 || height < 200) {
    std::cerr << "Invalid resolution: " << width << "x" << height
              << " (minimum 320x200)\n";
    return false;
  }
  if (width > 16384 || height > 16384) {
    std::cerr << "Resolution " << width << "x" << height
              << " exceeds the driver's hard ceiling (16384x16384)\n";
    return false;
  }
  if (vsync < 24000 || vsync > 1000000) {
    std::cerr << "Refresh rate " << (vsync / 1000) << " Hz out of range (24-1000 Hz)\n";
    return false;
  }
  return true;
}

static int CmdAdd(int argc, char *argv[]) {
  MonitorDesc desc = {};
  desc.Width = static_cast<UINT32>(std::stoul(argv[2]));
  desc.Height = static_cast<UINT32>(std::stoul(argv[3]));
  desc.VSync = (argc > 4) ? static_cast<UINT32>(std::stoul(argv[4])) : 60000;

  if (!ValidateMonitorGeometry(desc.Width, desc.Height, desc.VSync)) {
    return 1;
  }

  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. Is the driver installed? error=" << GetLastError() << "\n";
    return 1;
  }

  DWORD bytesReturned = 0;
  BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_ADD_MONITOR,
    &desc,
    sizeof(desc),
    &desc,
    sizeof(desc),
    &bytesReturned,
    nullptr
  );

  if (ok) {
    bool modeApplied = false;
    if (WaitForMonitorActive(desc.MonitorIndex, 8000)) {
      modeApplied = ApplyDesktopMode(desc);
    }
    if (g_JsonOutput) {
      std::cout << "{\"monitor_index\":" << desc.MonitorIndex
                << ",\"width\":" << desc.Width
                << ",\"height\":" << desc.Height
                << ",\"vsync\":" << desc.VSync
                << ",\"mode_applied\":" << (modeApplied ? "true" : "false")
                << "}\n";
    } else {
      std::cout << "Monitor added with index " << desc.MonitorIndex << " at "
                << desc.Width << "x" << desc.Height << "@" << desc.VSync << " mHz\n";
    }
    if (!modeApplied) {
      std::cerr << "Monitor created, but the requested mode could not be applied. "
                   "The display stack picked a default mode instead.\n";
    }
  } else {
    std::cerr << "IOCTL_VD_ADD_MONITOR failed: " << GetLastError() << "\n";
  }

  CloseHandle(hDevice);
  return ok ? 0 : 1;
}

static int CmdUpdate(int argc, char *argv[]) {
  MonitorDesc desc = {};
  desc.MonitorIndex = static_cast<UINT32>(std::stoul(argv[2]));
  desc.Width = static_cast<UINT32>(std::stoul(argv[3]));
  desc.Height = static_cast<UINT32>(std::stoul(argv[4]));
  desc.VSync = (argc > 5) ? static_cast<UINT32>(std::stoul(argv[5])) : 0;

  if (!ValidateMonitorGeometry(desc.Width, desc.Height, desc.VSync)) {
    return 1;
  }

  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. error=" << GetLastError() << "\n";
    return 1;
  }

  DWORD bytesReturned = 0;
  BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_UPDATE_MONITOR,
    &desc,
    sizeof(desc),
    &desc,
    sizeof(desc),
    &bytesReturned,
    nullptr
  );

  if (ok) {
    if (g_JsonOutput) {
      std::cout << "{\"monitor_index\":" << desc.MonitorIndex
                << ",\"width\":" << desc.Width
                << ",\"height\":" << desc.Height
                << ",\"vsync\":" << desc.VSync
                << "}\n";
    } else {
      std::cout << "Monitor " << desc.MonitorIndex << " updated to "
                << desc.Width << "x" << desc.Height << "@" << desc.VSync << " mHz\n";
    }
    if (!ApplyDesktopMode(desc)) {
      std::cerr << "Driver mode list updated, but desktop mode was not applied\n"
                << "The requested mode may not be in the monitor description table. "
                   "Try a mode listed by 'iddctrl displays'.\n";
      CloseHandle(hDevice);
      return 2;
    }
  } else {
    std::cerr << "IOCTL_VD_UPDATE_MONITOR failed: " << GetLastError() << "\n";
  }

  CloseHandle(hDevice);
  return ok ? 0 : 1;
}

static int CmdRemove(int, char *argv[]) {
  MonitorDesc desc = {};
  desc.MonitorIndex = static_cast<UINT32>(std::stoul(argv[2]));

  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. error=" << GetLastError() << "\n";
    return 1;
  }

  DWORD bytesReturned = 0;
  BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_REMOVE_MONITOR,
    &desc,
    sizeof(desc),
    nullptr,
    0,
    &bytesReturned,
    nullptr
  );

  if (ok) {
    if (g_JsonOutput) {
      std::cout << "{\"removed\":true,\"monitor_index\":" << desc.MonitorIndex << "}\n";
    } else {
      std::cout << "Monitor " << desc.MonitorIndex << " removed\n";
    }
  } else {
    std::cerr << "IOCTL_VD_REMOVE_MONITOR failed: " << GetLastError() << "\n";
  }

  CloseHandle(hDevice);
  return ok ? 0 : 1;
}

static int CmdClear() {
  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. error=" << GetLastError() << "\n";
    return 1;
  }

  DWORD bytesReturned = 0;
  BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_CLEAR_ALL_MONITORS,
    nullptr,
    0,
    nullptr,
    0,
    &bytesReturned,
    nullptr
  );

  if (ok) {
    if (g_JsonOutput) {
      std::cout << "{\"cleared\":true}\n";
    } else {
      std::cout << "All monitors removed\n";
    }
  } else {
    std::cerr << "IOCTL_VD_CLEAR_ALL_MONITORS failed: " << GetLastError() << "\n";
  }

  CloseHandle(hDevice);
  return ok ? 0 : 1;
}

static int CmdList() {
  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. error=" << GetLastError() << "\n";
    return 1;
  }

  UINT32 indexes[MAX_MONITORS] = {};
  DWORD bytesReturned = 0;
  BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_LIST_MONITORS,
    nullptr,
    0,
    indexes,
    sizeof(indexes),
    &bytesReturned,
    nullptr
  );

  if (ok) {
    const UINT32 count = bytesReturned / sizeof(UINT32);
    if (g_JsonOutput) {
      std::cout << "{\"monitor_count\":" << count << ",\"monitors\":[";
      for (UINT32 i = 0; i < count; i++) {
        if (i > 0) {
          std::cout << ",";
        }
        std::cout << indexes[i];
      }
      std::cout << "]}\n";
    } else {
      std::cout << "Active monitors: " << count << "\n";
      for (UINT32 i = 0; i < count; i++) {
        std::cout << "  index " << indexes[i] << "\n";
      }
    }
  } else {
    std::cerr << "IOCTL_VD_LIST_MONITORS failed: " << GetLastError() << "\n";
  }

  CloseHandle(hDevice);
  return ok ? 0 : 1;
}

static int CmdCaps() {
  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. error=" << GetLastError() << "\n";
    return 1;
  }

  IddCapabilityDesc caps = {};
  DWORD bytesReturned = 0;
  BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_GET_CAPABILITIES,
    nullptr,
    0,
    &caps,
    sizeof(caps),
    &bytesReturned,
    nullptr
  );

  if (ok) {
    if (g_JsonOutput) {
      std::cout << "{\"runtime_major\":" << caps.RuntimeMajor
                << ",\"runtime_minor\":" << caps.RuntimeMinor
                << ",\"runtime_revision\":" << caps.RuntimeRevision
                << ",\"capability_flags\":" << caps.CapabilityFlags
                << "}\n";
    } else {
      std::cout << "IddCx runtime: " << caps.RuntimeMajor << "."
                << caps.RuntimeMinor << " rev " << caps.RuntimeRevision << "\n";
      std::cout << "Capabilities:\n";
      std::cout << "  display_config_update: " << ((caps.CapabilityFlags & VD_CAP_DISPLAY_CONFIG_UPDATE) ? "yes" : "no") << "\n";
      std::cout << "  display_config_update2: " << ((caps.CapabilityFlags & VD_CAP_DISPLAY_CONFIG_UPDATE2) ? "yes" : "no") << "\n";
      std::cout << "  system_memory_swapchain: " << ((caps.CapabilityFlags & VD_CAP_SYSTEM_MEMORY_SWAPCHAIN) ? "yes" : "no") << "\n";
      std::cout << "  precise_present_regions: " << ((caps.CapabilityFlags & VD_CAP_PRECISE_PRESENT_REGIONS) ? "yes" : "no") << "\n";
      std::cout << "  hdr10: " << ((caps.CapabilityFlags & VD_CAP_HDR10) ? "yes" : "no") << "\n";
    }
  } else {
    std::cerr << "IOCTL_VD_GET_CAPABILITIES failed: " << GetLastError() << "\n";
  }

  CloseHandle(hDevice);
  return ok ? 0 : 1;
}

/**
 * @brief List DXGI graphics adapters available for rendering the virtual display.
 *
 * @return Zero when enumeration completed.
 */
static int CmdRenderList() {
  IDXGIFactory6 *factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    std::cerr << "CreateDXGIFactory1 failed hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    return 1;
  }

  bool firstJson = true;
  if (g_JsonOutput) {
    std::cout << "{\"adapters\":[";
  }
  for (UINT index = 0;; index++) {
    IDXGIAdapter1 *adapter = nullptr;
    hr = factory->EnumAdapterByGpuPreference(
      index,
      DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
      IID_PPV_ARGS(&adapter)
    );
    if (hr == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(hr)) {
      break;
    }

    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    adapter->Release();

    if (g_JsonOutput) {
      if (!firstJson) {
        std::cout << ",";
      }
      firstJson = false;
      std::cout << "{\"index\":" << index
                << ",\"vendor_id\":" << desc.VendorId
                << ",\"device_id\":" << desc.DeviceId
                << ",\"subsys_id\":" << desc.SubSysId
                << ",\"revision\":" << desc.Revision
                << ",\"luid\":\"" << desc.AdapterLuid.HighPart << ":" << desc.AdapterLuid.LowPart << "\""
                << ",\"software\":" << ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false")
                << ",\"description\":";
      JsonWriteStringW(desc.Description);
      std::cout << "}";
      continue;
    }

    std::cout << "adapter " << index << ": "
              << desc.Description
              << " vendor=0x" << std::hex << desc.VendorId
              << " device=0x" << desc.DeviceId
              << " subsys=0x" << desc.SubSysId
              << " rev=0x" << desc.Revision
              << std::dec
              << " luid=" << desc.AdapterLuid.HighPart << ":" << desc.AdapterLuid.LowPart
              << (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? " [software]" : "")
              << "\n";
  }
  if (g_JsonOutput) {
    std::cout << "]}\n";
  }
  factory->Release();
  return 0;
}

/**
 * @brief Query the driver's current render adapter selection.
 *
 * @return Zero when the driver state was read.
 */
static int CmdRenderGet() {
  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. error=" << GetLastError() << "\n";
    return 1;
  }

  RenderAdapterDesc status = {};
  status.Size = sizeof(status);
  DWORD bytesReturned = 0;
  const BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_GET_RENDER_ADAPTER,
    nullptr,
    0,
    &status,
    sizeof(status),
    &bytesReturned,
    nullptr
  );
  CloseHandle(hDevice);

  if (!ok || bytesReturned < sizeof(status)) {
    std::cerr << "IOCTL_VD_GET_RENDER_ADAPTER failed: " << GetLastError() << "\n";
    return 1;
  }

  if (g_JsonOutput) {
    std::cout << "{\"flags\":" << status.Flags
              << ",\"vendor_id\":" << status.VendorId
              << ",\"device_id\":" << status.DeviceId
              << ",\"subsys_id\":" << status.SubSysId
              << ",\"revision\":" << status.Revision
              << ",\"requested_luid\":\"" << status.RequestedLuid.HighPart << ":" << status.RequestedLuid.LowPart << "\""
              << ",\"actual_luid\":\"" << status.ActualLuid.HighPart << ":" << status.ActualLuid.LowPart << "\""
              << "}\n";
  } else {
    std::cout << "render_adapter:\n";
    std::cout << "  mode=" << ((status.Flags & VD_RENDER_AUTO) ? "auto" : "specific") << "\n";
    std::cout << "  vendor=0x" << std::hex << status.VendorId
              << " device=0x" << status.DeviceId
              << " subsys=0x" << status.SubSysId
              << " rev=0x" << status.Revision << std::dec << "\n";
    std::cout << "  requested_luid=" << status.RequestedLuid.HighPart << ":" << status.RequestedLuid.LowPart << "\n";
    if (status.Flags & VD_RENDER_ACTUAL_VALID) {
      std::cout << "  actual_luid=" << status.ActualLuid.HighPart << ":" << status.ActualLuid.LowPart << "\n";
    } else {
      std::cout << "  actual_luid=(not yet assigned)\n";
    }
  }
  return 0;
}

/**
 * @brief Set the driver's preferred render adapter.
 *
 * @param argc Command-line argument count.
 * @param argv Command-line arguments.
 * @return Zero when the driver accepted the request.
 */
static int CmdRenderSet(int argc, char *argv[]) {
  RenderAdapterDesc request = {};
  request.Size = sizeof(request);

  const std::string mode = argc >= 3 ? argv[2] : "auto";
  if (mode == "auto") {
    request.Flags = VD_RENDER_AUTO;
  } else if (mode == "id" && argc >= 5) {
    request.Flags = 0;
    request.VendorId = static_cast<UINT32>(std::stoul(argv[3], nullptr, 0));
    request.DeviceId = static_cast<UINT32>(std::stoul(argv[4], nullptr, 0));
    if (argc >= 6) {
      request.SubSysId = static_cast<UINT32>(std::stoul(argv[5], nullptr, 0));
    }
    if (argc >= 7) {
      request.Revision = static_cast<UINT32>(std::stoul(argv[6], nullptr, 0));
    }
    if (argc >= 9) {
      request.Flags |= VD_RENDER_LUID_HINT_VALID;
      request.RequestedLuid.HighPart = std::stol(argv[7]);
      request.RequestedLuid.LowPart = std::stoul(argv[8]);
    }
  } else {
    std::cerr << "Usage: iddctrl render-set auto\n"
                 "       iddctrl render-set id <vendor> <device> [subsys] [rev] [high low]\n";
    return 1;
  }

  HANDLE hDevice = OpenDevice();
  if (hDevice == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open device. error=" << GetLastError() << "\n";
    return 1;
  }

  RenderAdapterDesc status = {};
  status.Size = sizeof(status);
  DWORD bytesReturned = 0;
  const BOOL ok = DeviceIoControl(
    hDevice,
    IOCTL_VD_SET_RENDER_ADAPTER,
    &request,
    sizeof(request),
    &status,
    sizeof(status),
    &bytesReturned,
    nullptr
  );
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(hDevice);

  if (!ok || bytesReturned < sizeof(status)) {
    std::cerr << "IOCTL_VD_SET_RENDER_ADAPTER failed: " << error << "\n";
    return 1;
  }

  if (g_JsonOutput) {
    std::cout << "{\"flags\":" << status.Flags
              << ",\"vendor_id\":" << status.VendorId
              << ",\"device_id\":" << status.DeviceId
              << ",\"requested_luid\":\"" << status.RequestedLuid.HighPart << ":" << status.RequestedLuid.LowPart << "\""
              << "}\n";
  } else {
    std::cout << "Render adapter set: vendor=0x" << std::hex << status.VendorId
              << " device=0x" << status.DeviceId
              << " luid=" << std::dec << status.RequestedLuid.HighPart << ":" << status.RequestedLuid.LowPart << "\n";
    std::cout << "Note: Windows treats this as a preference; the actual swapchain\n"
                 "adapter is reported by 'iddctrl render-get' after a monitor is active.\n";
  }
  return 0;
}

/**
 * @brief Print SetupDi device paths and CreateFileW results.
 *
 * @param flags SetupDiGetClassDevs flags controlling which interfaces are enumerated.
 * @param label Diagnostic label.
 */
static void PrintSetupDiPaths(DWORD flags, const char *label) {
  std::cout << "SetupDi " << label << ":\n";
  HDEVINFO devInfo = SetupDiGetClassDevs(
    &GUID_DEVINTERFACE_VIRTUALDISPLAY,
    nullptr,
    nullptr,
    flags
  );

  if (devInfo == INVALID_HANDLE_VALUE) {
    std::cout << "  get_class_devs_error=" << GetLastError() << "\n";
    return;
  }

  for (DWORD index = 0;; index++) {
    SP_DEVICE_INTERFACE_DATA ifcData = {};
    ifcData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    BOOL found = SetupDiEnumDeviceInterfaces(
      devInfo,
      nullptr,
      &GUID_DEVINTERFACE_VIRTUALDISPLAY,
      index,
      &ifcData
    );
    if (!found) {
      std::cout << "  enum_end_error=" << GetLastError() << "\n";
      break;
    }

    DWORD bufSize = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifcData, nullptr, 0, &bufSize, nullptr);

    std::vector<BYTE> buf(bufSize);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
      reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    BOOL ok = SetupDiGetDeviceInterfaceDetailW(
      devInfo,
      &ifcData,
      detail,
      bufSize,
      nullptr,
      nullptr
    );
    if (!ok) {
      std::cout << "  detail_error=" << GetLastError() << "\n";
      continue;
    }

    DWORD openError = ERROR_SUCCESS;
    HANDLE hDevice = TryOpenDevicePath(detail->DevicePath, &openError);
    std::wcout << L"  path: " << detail->DevicePath;
    if (hDevice != INVALID_HANDLE_VALUE) {
      std::wcout << L" open=ok\n";
      CloseHandle(hDevice);
    } else {
      std::wcout << L" open_error=" << openError << L"\n";
    }
  }

  SetupDiDestroyDeviceInfoList(devInfo);
}

static int CmdPaths() {
  PrintSetupDiPaths(DIGCF_PRESENT | DIGCF_DEVICEINTERFACE, "present");
  PrintSetupDiPaths(DIGCF_DEVICEINTERFACE, "all");
  return 0;
}

/**
 * @brief Query Advanced Color state using the newest API supported by the OS.
 *
 * @param path Active DisplayConfig path to query.
 * @param hdrEnabled Receives whether HDR is enabled.
 * @param hdrSupported Receives whether HDR is supported.
 * @return Windows error code from the successful or final query.
 */
static LONG QueryHdrState(
  const DISPLAYCONFIG_PATH_INFO &path,
  bool *hdrEnabled,
  bool *hdrSupported
) {
  *hdrEnabled = false;
  *hdrSupported = false;

  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 colorInfo2 = {};
  colorInfo2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
  colorInfo2.header.size = sizeof(colorInfo2);
  colorInfo2.header.adapterId = path.targetInfo.adapterId;
  colorInfo2.header.id = path.targetInfo.id;
  LONG result = DisplayConfigGetDeviceInfo(&colorInfo2.header);
  if (result == ERROR_SUCCESS) {
    *hdrSupported = colorInfo2.highDynamicRangeSupported != 0;
    *hdrEnabled = colorInfo2.highDynamicRangeUserEnabled != 0;
    return ERROR_SUCCESS;
  }

  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colorInfo = {};
  colorInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  colorInfo.header.size = sizeof(colorInfo);
  colorInfo.header.adapterId = path.targetInfo.adapterId;
  colorInfo.header.id = path.targetInfo.id;
  result = DisplayConfigGetDeviceInfo(&colorInfo.header);
  if (result == ERROR_SUCCESS) {
    *hdrSupported = colorInfo.advancedColorSupported != 0;
    *hdrEnabled = colorInfo.advancedColorEnabled != 0;
  }
  return result;
}

/**
 * @brief Query or change HDR state for the active VirtualDisplay path.
 *
 * @param action "status", "on", or "off".
 * @return Zero when the query or requested state change succeeds.
 */
static int CmdAdvancedColor(const std::string &action, UINT32 index = 1) {
  DISPLAYCONFIG_PATH_INFO path = {};
  if (!FindActiveVirtualDisplayConfigPath(&path, index)) {
    std::cerr << "No active VirtualDisplay DisplayConfig path found for monitor " << index
              << ". Add and activate a monitor first.\n";
    return 1;
  }

  bool hdrSupported = false;
  bool hdrEnabled = false;
  LONG result = QueryHdrState(path, &hdrEnabled, &hdrSupported);
  if (result != ERROR_SUCCESS) {
    std::cerr << "DisplayConfigGetDeviceInfo advanced color failed: " << result << "\n";
    return 1;
  }

  if (action == "on" || action == "off") {
    const bool enable = action == "on";

    DISPLAYCONFIG_SET_HDR_STATE hdrState = {};
    hdrState.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
    hdrState.header.size = sizeof(hdrState);
    hdrState.header.adapterId = path.targetInfo.adapterId;
    hdrState.header.id = path.targetInfo.id;
    hdrState.enableHdr = enable ? 1 : 0;
    result = DisplayConfigSetDeviceInfo(&hdrState.header);
    if (result != ERROR_SUCCESS) {
      DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE colorState = {};
      colorState.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
      colorState.header.size = sizeof(colorState);
      colorState.header.adapterId = path.targetInfo.adapterId;
      colorState.header.id = path.targetInfo.id;
      colorState.enableAdvancedColor = enable ? 1 : 0;
      result = DisplayConfigSetDeviceInfo(&colorState.header);
    }
    if (result != ERROR_SUCCESS) {
      std::cerr << "DisplayConfigSetDeviceInfo HDR state failed: " << result << "\n";
      if (enable && !hdrSupported) {
        std::cerr << "HDR is not supported on this path. The driver must be built with IddCx 1.10\n"
                     "HDR support and the monitor EDID must carry HDR metadata.\n";
      }
      return 1;
    }

    result = QueryHdrState(path, &hdrEnabled, &hdrSupported);
    if (result != ERROR_SUCCESS) {
      std::cerr << "Advanced color re-query failed: " << result << "\n";
      return 1;
    }
    if (enable && !hdrEnabled) {
      std::cerr << "HDR request was accepted but did not activate. The display stack may not\n"
                   "support HDR on this machine (e.g. virtualized GPU, remote session).\n";
      return 2;
    }
  }

  if (g_JsonOutput) {
    std::cout << "{\"hdr_supported\":" << (hdrSupported ? "true" : "false")
              << ",\"hdr_enabled\":" << (hdrEnabled ? "true" : "false")
              << "}\n";
  } else {
    std::cout << "hdr_supported=" << (hdrSupported ? "yes" : "no") << "\n";
    std::cout << "hdr_enabled=" << (hdrEnabled ? "yes" : "no") << "\n";
  }
  return 0;
}

/**
 * @brief Dump advanced color diagnostics for every active DisplayConfig path.
 *
 * Prints adapter id, target id, output technology, and both generations of
 * the advanced color info (INFO_1 and INFO_2) for each active path.
 */
static int CmdAdvColorDebug() {
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
  if (result != ERROR_SUCCESS) {
    std::cerr << "GetDisplayConfigBufferSizes failed: " << result << "\n";
    return 1;
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  result = QueryDisplayConfig(
    QDC_ONLY_ACTIVE_PATHS,
    &pathCount,
    paths.data(),
    &modeCount,
    modes.data(),
    nullptr
  );
  if (result != ERROR_SUCCESS) {
    std::cerr << "QueryDisplayConfig failed: " << result << "\n";
    return 1;
  }

  if (g_JsonOutput) {
    std::cout << "{\"paths\":[";
  }
  bool first = true;
  for (UINT32 i = 0; i < pathCount; i++) {
    const DISPLAYCONFIG_PATH_INFO &p = paths[i];

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info2 = {};
    info2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    info2.header.size = sizeof(info2);
    info2.header.adapterId = p.targetInfo.adapterId;
    info2.header.id = p.targetInfo.id;
    LONG r2 = DisplayConfigGetDeviceInfo(&info2.header);

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info1 = {};
    info1.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info1.header.size = sizeof(info1);
    info1.header.adapterId = p.targetInfo.adapterId;
    info1.header.id = p.targetInfo.id;
    LONG r1 = DisplayConfigGetDeviceInfo(&info1.header);

    const LONG longAdapter = p.targetInfo.adapterId.HighPart;
    const DWORD lowAdapter = p.targetInfo.adapterId.LowPart;

    if (g_JsonOutput) {
      if (!first) std::cout << ",";
      std::cout << "{\"adapterId\":\"" << longAdapter << ":" << lowAdapter
                << "\",\"id\":" << p.targetInfo.id
                << ",\"tech\":" << p.targetInfo.outputTechnology
                << ",\"flags\":0x" << std::hex << p.flags << std::dec
                << ",\"ret1\":" << r1
                << ",\"advSup\":" << (r1 == ERROR_SUCCESS && info1.advancedColorSupported ? 1 : 0)
                << ",\"advEn\":" << (r1 == ERROR_SUCCESS && info1.advancedColorEnabled ? 1 : 0)
                << ",\"ret2\":" << r2
                << ",\"hdrSup\":" << (r2 == ERROR_SUCCESS && info2.highDynamicRangeSupported ? 1 : 0)
                << ",\"hdrUserEn\":" << (r2 == ERROR_SUCCESS && info2.highDynamicRangeUserEnabled ? 1 : 0)
                << "}";
      first = false;
    } else {
      std::cout << "path[" << i << "] adapter=" << longAdapter << ":" << lowAdapter
                << " id=" << p.targetInfo.id
                << " tech=" << p.targetInfo.outputTechnology
                << " flags=0x" << std::hex << p.flags << std::dec << "\n";
      std::cout << "  INFO_1 ret=" << r1
                << " advancedColorSupported=" << (r1 == ERROR_SUCCESS && info1.advancedColorSupported ? 1 : 0)
                << " advancedColorEnabled=" << (r1 == ERROR_SUCCESS && info1.advancedColorEnabled ? 1 : 0) << "\n";
      std::cout << "  INFO_2 ret=" << r2
                << " hdrSupported=" << (r2 == ERROR_SUCCESS && info2.highDynamicRangeSupported ? 1 : 0)
                << " hdrUserEnabled=" << (r2 == ERROR_SUCCESS && info2.highDynamicRangeUserEnabled ? 1 : 0) << "\n";
    }
  }
  if (g_JsonOutput) {
    std::cout << "]}\n";
  }
  return 0;
}

/**
 * @brief Capture one frame from a DXGI output duplication path.
 *
 * @param argc Command-line argument count.
 * @param argv Command-line arguments.
 * @return Zero when a frame was captured.
 */
static int CmdDxgiCapture(int argc, char *argv[]) {
  std::wstring targetDisplay = (argc >= 3) ? Utf8ToWide(argv[2]) : FindActiveVirtualDisplayName();
  if (targetDisplay.empty()) {
    std::cerr << "No VirtualDisplay found. Activate the virtual display first.\n";
    return 1;
  }

  std::wcout << L"target_display=" << targetDisplay << L"\n";

  IDXGIFactory1 *factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
  if (FAILED(hr)) {
    std::cout << "CreateDXGIFactory1 failed hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    return 1;
  }

  int result = 1;
  for (UINT adapterIndex = 0; result != 0; adapterIndex++) {
    IDXGIAdapter1 *adapter = nullptr;
    hr = factory->EnumAdapters1(adapterIndex, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(hr)) {
      break;
    }

    for (UINT outputIndex = 0; result != 0; outputIndex++) {
      IDXGIOutput *output = nullptr;
      hr = adapter->EnumOutputs(outputIndex, &output);
      if (hr == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(hr)) {
        break;
      }

      DXGI_OUTPUT_DESC outputDesc = {};
      output->GetDesc(&outputDesc);
      if (_wcsicmp(outputDesc.DeviceName, targetDisplay.c_str()) != 0) {
        ReleaseIfSet(&output);
        continue;
      }

      IDXGIOutput1 *output1 = nullptr;
      hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&output1));
      ReleaseIfSet(&output);
      if (FAILED(hr)) {
        break;
      }

      ID3D11Device *device = nullptr;
      ID3D11DeviceContext *context = nullptr;
      hr = CreateD3D11DeviceForAdapter(adapter, &device, &context);
      if (FAILED(hr)) {
        ReleaseIfSet(&output1);
        break;
      }

      result = CaptureOneDxgiFrame(output1, device, context);
      ReleaseIfSet(&context);
      ReleaseIfSet(&device);
      ReleaseIfSet(&output1);
    }

    ReleaseIfSet(&adapter);
  }

  ReleaseIfSet(&factory);
  if (result != 0) {
    std::wcerr << L"DXGI capture failed for " << targetDisplay << L"\n";
  }
  return result;
}

/**
 * @brief Install a certificate file into a Windows certificate store.
 *
 * @param file Certificate file path.
 * @param store Store name such as "Root" or "TrustedPublisher".
 * @return True when the certificate was installed.
 */
static bool InstallCertificate(const wchar_t *file, const wchar_t *store) {
  std::wstring command = L"certutil -addstore ";
  command += store;
  command += L" \"";
  command += file;
  command += L"\"";

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  BOOL ok = CreateProcessW(
    nullptr,
    command.data(),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    nullptr,
    &startup,
    &process
  );
  if (!ok) {
    return false;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exitCode == 0;
}

/**
 * @brief Locate the VirtualDisplay INF next to this executable or in the working directory.
 *
 * @return Absolute path to the INF, or an empty string when not found.
 */
static std::wstring FindInfPath(const std::wstring &explicitPath) {
  if (!explicitPath.empty()) {
    return explicitPath;
  }

  const wchar_t *candidates[] = {
    L"VirtualDisplay.inf",
  };

  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));

  std::wstring moduleDir(modulePath);
  const auto slash = moduleDir.find_last_of(L'\\');
  if (slash != std::wstring::npos) {
    moduleDir.resize(slash + 1);
  }

  for (const wchar_t *candidate : candidates) {
    const std::wstring moduleCandidate = moduleDir + candidate;
    if (GetFileAttributesW(moduleCandidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return moduleCandidate;
    }
    if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) {
      return candidate;
    }
  }
  return std::wstring();
}

/**
 * @brief Convert a narrow command-line string to a wide string.
 *
 * @param value Narrow command-line value.
 * @return Wide string, or an empty string when conversion fails.
 */
static std::wstring ToWideString(const char *value) {
  if (!value) {
    return {};
  }

  const int required = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
  if (required <= 0) {
    return {};
  }

  std::wstring output(static_cast<size_t>(required), L'\0');
  MultiByteToWideChar(CP_ACP, 0, value, -1, output.data(), required);
  output.resize(static_cast<size_t>(required - 1));
  return output;
}

/**
 * @brief Print a Win32 or SetupAPI error code with its formatted message.
 *
 * @param prefix Text that identifies the failed operation.
 * @param error Error code returned by `GetLastError`.
 */
static void PrintError(const char *prefix, DWORD error) {
  wchar_t message[512] = {};
  HMODULE setupApi = LoadLibraryW(L"setupapi.dll");
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;

  if (error >= 0xE0000000 && setupApi) {
    flags = FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;
  }

  const DWORD chars = FormatMessageW(
    flags,
    error >= 0xE0000000 ? setupApi : nullptr,
    error,
    0,
    message,
    ARRAYSIZE(message),
    nullptr);

  printf("%s failed: %lu (0x%08lX)", prefix, error, error);
  if (chars) {
    printf(" - %ls", message);
  }
  printf("\n");

  if (setupApi) {
    FreeLibrary(setupApi);
  }
}

/**
 * @brief Build a MULTI_SZ payload for SPDRP_HARDWAREID.
 *
 * @param hardwareId Single hardware ID to write.
 * @return Multi-string containing the ID and a final empty string terminator.
 */
static std::wstring BuildHardwareIdMultiSz(const std::wstring &hardwareId) {
  std::wstring multi = hardwareId;
  multi.push_back(L'\0');
  multi.push_back(L'\0');
  return multi;
}

/**
 * @brief Install the VirtualDisplay driver package and create the root-enumerated device.
 *
 * @param argc Command-line argument count.
 * @param argv Command-line arguments.
 * @return Zero on success, otherwise a process error code.
 */
static int CmdInstall(int argc, char *argv[]) {
  if (!IsElevated()) {
    std::wstring args = L"install";
    for (int i = 2; i < argc; i++) {
      args += L" ";
      args += Utf8ToWide(argv[i]);
    }
    std::cout << "Relaunching elevated for install...\n";
    return RunElevated(args.c_str());
  }

  if (!IsTestSigningEnabled()) {
    std::cerr
        << "Note: Windows test-signing mode is OFF.\n"
           "Installation can still succeed when the driver catalog is signed "
           "by a certificate trusted in Root and TrustedPublisher (see "
           "--trust-certs).\n";
  }

  std::wstring infPath;
  bool trustCerts = false;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--inf") == 0 && i + 1 < argc) {
      infPath = ToWideString(argv[++i]);
    } else if (strcmp(argv[i], "--trust-certs") == 0) {
      trustCerts = true;
    } else {
      std::cerr << "Unknown install option: " << argv[i] << "\n";
      std::cerr << "Full argv dump:";
      for (int a = 0; a < argc; a++) {
        std::cerr << " [" << a << "]=" << argv[a];
      }
      std::cerr << "\n";
      PrintUsage();
      return 1;
    }
  }

  infPath = FindInfPath(infPath);
  if (infPath.empty()) {
    std::cerr << "VirtualDisplay.inf not found next to iddctrl.exe. Use --inf <path>.\n";
    return 1;
  }

  // SetupCopyOEMInf resolves CatalogFile= relative to the INF directory.
  // Copy the catalog next to the INF when it is missing (mirrors the
  // Sunshine install flow, which stages the catalog beside the INF first).
  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
  std::wstring moduleDir(modulePath);
  const auto moduleSlash = moduleDir.find_last_of(L'\\');
  if (moduleSlash != std::wstring::npos) {
    moduleDir.resize(moduleSlash + 1);
  }

  std::wstring infDir(infPath);
  const auto infSlash = infDir.find_last_of(L'\\');
  if (infSlash != std::wstring::npos) {
    infDir.resize(infSlash + 1);
  } else {
    infDir.clear();
  }
  const std::wstring catName = L"VirtualDisplay.cat";
  const std::wstring infSideCat = infDir + catName;
  if (GetFileAttributesW(infSideCat.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const std::wstring moduleCat = moduleDir + catName;
    if (GetFileAttributesW(moduleCat.c_str()) != INVALID_FILE_ATTRIBUTES &&
        CopyFileW(moduleCat.c_str(), infSideCat.c_str(), FALSE)) {
      std::wcout << L"  Catalog copied next to INF: " << infSideCat << L"\n";
    }
  }

  if (trustCerts) {
    std::wcout << L"Installing certificates...\n";
    const std::wstring rootCert = moduleDir + VD_ROOT_CERT_NAME;
    const std::wstring leafCert = moduleDir + VD_LEAF_CERT_NAME;
    if (GetFileAttributesW(rootCert.c_str()) != INVALID_FILE_ATTRIBUTES) {
      std::wcout << L"  Root CA: " << (InstallCertificate(rootCert.c_str(), L"Root") ? L"installed" : L"failed") << L"\n";
    }
    if (GetFileAttributesW(leafCert.c_str()) != INVALID_FILE_ATTRIBUTES) {
      std::wcout << L"  Signing cert: " << (InstallCertificate(leafCert.c_str(), L"TrustedPublisher") ? L"installed" : L"failed") << L"\n";
    }
  }

  if (g_JsonOutput) {
    std::cerr << "Step 1: Staging INF into the DriverStore...\n";
  } else {
    std::cout << "Step 1: Staging INF into the DriverStore...\n";
  }
  if (!SetupCopyOEMInfW(
        infPath.c_str(),
        nullptr,
        SPOST_PATH,
        SP_COPY_NEWER_OR_SAME,
        nullptr,
        0,
        nullptr,
        nullptr)) {
    DWORD err = GetLastError();
    if (err != ERROR_FILE_EXISTS && err != ERROR_SUCCESS) {
      PrintError("SetupCopyOEMInf", err);
      return 1;
    }
    if (g_JsonOutput) {
      std::cerr << "  INF already in driver store (code=" << err << ").\n";
    } else {
      std::cout << "  INF already in driver store (code=" << err << ").\n";
    }
  } else {
    if (g_JsonOutput) {
      std::cerr << "  INF staged.\n";
    } else {
      std::cout << "  INF staged.\n";
    }
  }

  std::wstring hardwareId = VD_ROOT_HARDWARE_ID;
  std::wstring deviceName = VD_DEVICE_NAME;

  if (g_JsonOutput) {
    std::cerr << "Step 2: Creating root-enumerated device...\n";
  } else {
    std::cout << "Step 2: Creating root-enumerated device...\n";
  }

  HDEVINFO devInfo = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (devInfo == INVALID_HANDLE_VALUE) {
    PrintError("SetupDiCreateDeviceInfoList", GetLastError());
    return 1;
  }

  SP_DEVINFO_DATA devData = {};
  devData.cbSize = sizeof(devData);

  const std::wstring deviceNodeName = L"VIRTUALDISPLAY";
  const std::wstring hwId = BuildHardwareIdMultiSz(hardwareId);

  if (!SetupDiCreateDeviceInfoW(
        devInfo,
        deviceNodeName.c_str(),
        const_cast<GUID *>(&GUID_DEVCLASS_DISPLAY),
        deviceName.c_str(),
        nullptr,
        DICD_GENERATE_ID,
        &devData)) {
    PrintError("SetupDiCreateDeviceInfo", GetLastError());
    SetupDiDestroyDeviceInfoList(devInfo);
    return 1;
  }

  if (!SetupDiSetDeviceRegistryPropertyW(
        devInfo,
        &devData,
        SPDRP_HARDWAREID,
        reinterpret_cast<const BYTE *>(hwId.c_str()),
        static_cast<DWORD>(hwId.size() * sizeof(wchar_t)))) {
    PrintError("SetupDiSetDeviceRegistryProperty", GetLastError());
    SetupDiDestroyDeviceInfoList(devInfo);
    return 1;
  }

  if (!SetupDiRegisterDeviceInfo(
        devInfo,
        &devData,
        0,
        nullptr,
        nullptr,
        nullptr)) {
    PrintError("SetupDiRegisterDeviceInfo", GetLastError());
    SetupDiDestroyDeviceInfoList(devInfo);
    return 1;
  }

  wchar_t instanceId[MAX_PATH] = {};
  if (SetupDiGetDeviceInstanceIdW(devInfo, &devData, instanceId, ARRAYSIZE(instanceId), nullptr)) {
    if (g_JsonOutput) {
      std::wcerr << L"Device instance: " << instanceId << L"\n";
    } else {
      std::wcout << L"Device instance: " << instanceId << L"\n";
    }
  }

  SetupDiDestroyDeviceInfoList(devInfo);

  if (g_JsonOutput) {
    std::cerr << "Step 3: Binding signed driver package...\n";
  } else {
    std::cout << "Step 3: Binding signed driver package...\n";
  }
  BOOL rebootRequired = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(
        nullptr,
        hardwareId.c_str(),
        infPath.c_str(),
        INSTALLFLAG_FORCE,
        &rebootRequired)) {
    PrintError("UpdateDriverForPlugAndPlayDevices", GetLastError());
    return 1;
  }

  if (g_JsonOutput) {
    std::cerr << "  Driver package installed.\n";
  } else {
    std::cout << "  Driver package installed.\n";
  }
  if (rebootRequired) {
    if (g_JsonOutput) {
      std::cerr << "  Reboot required before the device can start.\n";
    } else {
      std::cout << "  Reboot required before the device can start.\n";
    }
  }

  if (g_JsonOutput) {
    std::cout << "{\"installed\":true,\"device\":\"ROOT\\\\VIRTUALDISPLAY\",\"reboot_required\":"
              << (rebootRequired ? "true" : "false") << "}\n";
  } else {
    std::cout << "Done. Use 'iddctrl caps' and 'iddctrl add' to verify.\n";
  }
  return 0;
}

/**
 * @brief Remove the VirtualDisplay root device and driver package.
 *
 * @return Zero on success, otherwise a process error code.
 */
static int CmdUninstall() {
  if (!IsElevated()) {
    std::cout << "Relaunching elevated for uninstall...\n";
    return RunElevated(L"uninstall");
  }

  int exitCode = 0;

  if (g_JsonOutput) {
    std::cerr << "Step 1: Removing root-enumerated device...\n";
  } else {
    std::cout << "Step 1: Removing root-enumerated device...\n";
  }
  HDEVINFO devInfo = SetupDiGetClassDevs(
    &GUID_DEVCLASS_DISPLAY,
    nullptr,
    nullptr,
    DIGCF_PRESENT
  );
  if (devInfo != INVALID_HANDLE_VALUE) {
    bool removed = false;
    for (DWORD index = 0;; index++) {
      SP_DEVINFO_DATA devData = {};
      devData.cbSize = sizeof(devData);
      if (!SetupDiEnumDeviceInfo(devInfo, index, &devData)) {
        break;
      }

      wchar_t instanceId[MAX_PATH] = {};
      if (!SetupDiGetDeviceInstanceIdW(devInfo, &devData, instanceId, ARRAYSIZE(instanceId), nullptr)) {
        continue;
      }
      if (wcsstr(instanceId, L"ROOT\\VIRTUALDISPLAY") == nullptr) {
        continue;
      }

      if (g_JsonOutput) {
        std::wcerr << L"  Removing device " << instanceId << L"...\n";
      } else {
        std::wcout << L"  Removing device " << instanceId << L"...\n";
      }
      SetupDiSetClassInstallParamsW(        devInfo,
        &devData,
        nullptr,
        0
      );
      SP_REMOVEDEVICE_PARAMS removeParams = {};
      removeParams.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
      removeParams.ClassInstallHeader.InstallFunction = DIF_REMOVE;
      removeParams.Scope = DI_REMOVEDEVICE_GLOBAL;
      removeParams.HwProfile = 0;
      if (SetupDiSetClassInstallParamsW(
            devInfo,
            &devData,
            &removeParams.ClassInstallHeader,
            sizeof(removeParams)) &&
          SetupDiCallClassInstaller(DIF_REMOVE, devInfo, &devData)) {
        if (g_JsonOutput) {
          std::wcerr << L"  Device removed.\n";
        } else {
          std::wcout << L"  Device removed.\n";
        }
        removed = true;
      } else {
        PrintError("SetupDiCallClassInstaller(DIF_REMOVE)", GetLastError());
        exitCode = 1;
      }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    if (!removed) {
      if (g_JsonOutput) {
        std::wcerr << L"  No ROOT\\VIRTUALDISPLAY device found.\n";
      } else {
        std::wcout << L"  No ROOT\\VIRTUALDISPLAY device found.\n";
      }
    }
  }

  if (g_JsonOutput) {
    std::cerr << "Step 2: Removing driver package from the DriverStore...\n";
  } else {
    std::cout << "Step 2: Removing driver package from the DriverStore...\n";
  }

  // Resolve the published (oemXX.inf) name from pnputil before deleting.
  // /delete-driver with the original INF name is unreliable on modern
  // Windows (the DriverStore keeps the package under its published name).
  std::wstring deleteTarget = L"VirtualDisplay.inf";
  {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE enumRead = nullptr, enumWrite = nullptr;
    CreatePipe(&enumRead, &enumWrite, &sa, 0);
    if (enumRead) {
      SetHandleInformation(enumRead, HANDLE_FLAG_INHERIT, 0);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = enumWrite;
      si.hStdError = enumWrite;
      si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::wstring enumCmd = L"pnputil /enum-drivers";
    if (CreateProcessW(nullptr, enumCmd.data(),
                       nullptr, nullptr, enumRead ? TRUE : FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      if (enumWrite) CloseHandle(enumWrite);
      std::string enumOut;
      char buf[2048];
      DWORD n = 0;
      while (WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT) {
        while (enumRead && PeekNamedPipe(enumRead, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
          if (!ReadFile(enumRead, buf, sizeof(buf), &n, nullptr) || n == 0) break;
          enumOut.append(buf, n);
        }
      }
      if (enumRead) {
        while (ReadFile(enumRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
          enumOut.append(buf, n);
        }
        CloseHandle(enumRead);
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);

      // pnputil lists each package with "oemNN.inf" (published name) and the
      // original name on a separate line. Find the oemNN.inf published name
      // that belongs to the package whose original name is virtualdisplay.inf.
      const std::string marker = "virtualdisplay.inf";
      size_t pos = 0;
      while ((pos = enumOut.find(marker, pos)) != std::string::npos) {
        // Search backwards for the most recent "oemNN.inf" occurrence.
        size_t oemPos = enumOut.rfind("oem", pos);
        bool found = false;
        while (oemPos != std::string::npos) {
          const size_t dot = enumOut.find(".inf", oemPos);
          if (dot != std::string::npos && dot < pos && dot - oemPos < 12) {
            deleteTarget = Utf8ToWide(enumOut.substr(oemPos, dot + 4 - oemPos).c_str());
            found = true;
            break;
          }
          if (oemPos == 0) {
            break;
          }
          oemPos = enumOut.rfind("oem", oemPos - 1);
        }
        if (found) {
          break;
        }
        pos += marker.size();
      }
    } else {
      if (enumRead) CloseHandle(enumRead);
      if (enumWrite) CloseHandle(enumWrite);
    }
  }

  std::wstring command = L"pnputil /delete-driver " + deleteTarget + L" /uninstall /force";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE readPipe = nullptr, writePipe = nullptr;
  CreatePipe(&readPipe, &writePipe, &sa, 0);
  if (readPipe) {
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  BOOL ok = CreateProcessW(
    nullptr,
    command.data(),
    nullptr,
    nullptr,
    readPipe ? TRUE : FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    nullptr,
    &startup,
    &process
  );
  if (writePipe) CloseHandle(writePipe);
  if (ok) {
    const DWORD kPnpTimeoutMs = 30000;
    std::string pnpOutput;
    char buf[2048];
    DWORD n = 0;
    const ULONGLONG startedAt = GetTickCount64();
    bool timedOut = false;
    while (true) {
      while (readPipe && PeekNamedPipe(readPipe, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
        if (!ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        pnpOutput.append(buf, n);
      }
      DWORD wait = WaitForSingleObject(process.hProcess, 50);
      if (wait == WAIT_OBJECT_0) break;
      if (wait == WAIT_TIMEOUT &&
          GetTickCount64() - startedAt > kPnpTimeoutMs) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        timedOut = true;
        break;
      }
    }
    if (readPipe) {
      while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        pnpOutput.append(buf, n);
      }
      CloseHandle(readPipe);
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (g_JsonOutput) {
      std::cerr << "  pnputil exit code: " << code
                << (timedOut ? " (timed out, terminated)" : "") << "\n";
    } else {
      std::cout << "  pnputil exit code: " << code
                << (timedOut ? " (timed out, terminated)" : "") << "\n";
    }
    if (!pnpOutput.empty()) {
      if (g_JsonOutput) {
        std::cerr << "  pnputil output:\n" << pnpOutput;
      } else {
        std::cout << "  pnputil output:\n" << pnpOutput;
      }
    }
  } else {
    std::cerr << "  pnputil launch failed: " << GetLastError() << "\n";
  }

  if (g_JsonOutput) {
    std::cout << "{\"uninstalled\":true}\n";
  } else {
    std::cout << "Done.\n";
  }
  return exitCode;
}

/**
 * @brief Read the current monitor layout and write it to a config file.
 *
 * The layout is collected through the GDI display adapter list: every active
 * VirtualDisplay adapter contributes one monitor entry.
 *
 * @param filePath Destination file path.
 * @return Zero on success, otherwise a process error code.
 */
static int CmdSaveConfig(const std::wstring &filePath) {
  const std::wstring path = filePath.empty() ? VD_CONFIG_FILE : filePath;

  CreateDirectoryW(VD_CONFIG_DIR, nullptr);

  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    std::cerr << "Failed to open config file for writing: " << GetLastError() << "\n";
    return 1;
  }

  UINT32 saved = 0;
  for (DWORD index = 0;; index++) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) {
      break;
    }
    if (!IsVirtualDisplayDevice(adapter)) {
      continue;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
      continue;
    }

    output << mode.dmPelsWidth << " " << mode.dmPelsHeight
           << " " << mode.dmDisplayFrequency * VSYNC_MHZ_DENOMINATOR << "\n";
    saved++;
  }

  output.close();
  if (!output.good()) {
    std::cerr << "Failed to flush config file\n";
    return 1;
  }

  std::wcout << L"Saved " << saved << L" monitor(s) to " << path << L"\n";
  return 0;
}

/**
 * @brief Restore the monitor layout from a config file.
 *
 * Each line is "width height vsync_mhz". The tool clears existing monitors,
 * then re-creates each entry with the stored geometry.
 *
 * @param filePath Source file path.
 * @return Zero on success, otherwise a process error code.
 */
static int CmdRestore(const std::wstring &filePath) {
  const std::wstring path = filePath.empty() ? VD_CONFIG_FILE : filePath;

  std::ifstream input(path);
  if (!input) {
    std::cerr << "Config file not found: ";
    std::wcerr << path << L"\n";
    return 1;
  }

  struct SavedMonitor {
    UINT32 Width;
    UINT32 Height;
    UINT32 VSync;
  };
  std::vector<SavedMonitor> monitors;
  UINT32 width = 0;
  UINT32 height = 0;
  UINT32 vsync = 0;
  while (input >> width >> height >> vsync) {
    monitors.push_back({width, height, vsync});
  }
  input.close();

  if (monitors.empty()) {
    std::cerr << "Config file contains no monitor entries\n";
    return 1;
  }

  std::cout << "Clearing existing monitors...\n";
  CmdClear();

  for (const auto &monitor : monitors) {
    MonitorDesc desc = {};
    desc.Width = monitor.Width;
    desc.Height = monitor.Height;
    desc.VSync = monitor.VSync;

    HANDLE hDevice = OpenDevice();
    if (hDevice == INVALID_HANDLE_VALUE) {
      std::cerr << "Failed to open device. Is the driver installed? error=" << GetLastError() << "\n";
      return 1;
    }

    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(
      hDevice,
      IOCTL_VD_ADD_MONITOR,
      &desc,
      sizeof(desc),
      &desc,
      sizeof(desc),
      &bytesReturned,
      nullptr
    );
    CloseHandle(hDevice);

    if (!ok) {
      std::cerr << "IOCTL_VD_ADD_MONITOR failed: " << GetLastError() << "\n";
      return 1;
    }

    std::cout << "Restored monitor " << desc.MonitorIndex << " at "
              << desc.Width << "x" << desc.Height << "@" << desc.VSync << " mHz\n";
    ApplyDesktopMode(desc);
  }

  std::wcout << L"Restored " << monitors.size() << L" monitor(s) from " << path << L"\n";
  return 0;
}

/**
 * @brief Run a command line and wait for it to finish.
 *
 * @param command Command line (Unicode).
 * @return Process exit code, or 0xFFFFFFFF on spawn failure.
 */
static DWORD RunShellCommand(const std::wstring &command) {
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  BOOL ok = CreateProcessW(
    nullptr,
    const_cast<wchar_t *>(command.c_str()),
    nullptr,
    nullptr,
    FALSE,
    CREATE_NO_WINDOW,
    nullptr,
    nullptr,
    &startup,
    &process
  );
  if (!ok) {
    return 0xFFFFFFFF;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exitCode;
}

/**
 * @brief Register or remove the "开机自恢复" scheduled task.
 *
 * The task launches the GUI with --restore at logon, which restores the
 * saved monitor layout. Registering requires the task name and the GUI exe
 * path next to this control utility.
 *
 * @param enable True to register, false to unregister.
 * @return Zero on success.
 */
static int CmdRegisterTask(bool enable) {
  wchar_t exePath[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath))) {
    std::cerr << "Failed to locate iddctrl.exe\n";
    return 1;
  }
  PathRemoveFileSpecW(exePath);
  std::wstring baseDir = exePath;

  // Locate the GUI executable. Candidates: same directory as iddctrl, the
  // parent "gui" sibling directory (installed layouts), or ProgramData.
  std::vector<std::wstring> candidates;
  candidates.push_back(baseDir + L"\\VirtualDisplay.exe");
  candidates.push_back(baseDir + L"\\gui\\VirtualDisplay.exe");
  const size_t lastSep = baseDir.find_last_of(L"\\/");
  if (lastSep != std::wstring::npos) {
    const std::wstring parent = baseDir.substr(0, lastSep);
    candidates.push_back(parent + L"\\gui\\VirtualDisplay.exe");
  }
  candidates.push_back(L"C:\\ProgramData\\VirtualDisplay\\VirtualDisplay.exe");

  std::wstring guiExe;
  for (const auto &candidate : candidates) {
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
      guiExe = candidate;
      break;
    }
  }

  if (!enable) {
    std::wstring unreg = L"schtasks /Delete /TN \"VirtualDisplayAutoRestore\" /F";
    const DWORD code = RunShellCommand(unreg);
    if (g_JsonOutput) {
      std::cout << "{\"registered\":false,\"removed\":true,\"exit\":" << code << "}\n";
    } else {
      std::cout << (code == 0 ? "Auto-restore task removed\n" : "Failed to remove auto-restore task\n");
    }
    return code == 0 ? 0 : 1;
  }

  if (guiExe.empty()) {
    std::cerr << "GUI executable not found near iddctrl.exe\n";
    return 1;
  }

  // At logon, for the current user, with highest privileges (drivers need admin).
  std::wstring reg = L"schtasks /Create /TN \"VirtualDisplayAutoRestore\" /TR \"\\\"";
  reg += guiExe;
  reg += L"\\\" --restore\" /SC ONLOGON /RL HIGHEST /F";
  const DWORD code = RunShellCommand(reg);

  if (g_JsonOutput) {
    std::cout << "{\"registered\":" << (code == 0 ? "true" : "false")
              << ",\"exit\":" << code << "}\n";
  } else {
    std::cout << (code == 0 ? "Auto-restore task registered\n" : "Failed to register auto-restore task\n");
  }
  return code == 0 ? 0 : 1;
}

/**
 * @brief Query whether the auto-restore scheduled task exists.
 *
 * @return Zero when the task exists.
 */
static int CmdTaskStatus() {
  const std::wstring query = L"schtasks /Query /TN \"VirtualDisplayAutoRestore\"";
  const DWORD code = RunShellCommand(query);
  if (g_JsonOutput) {
    std::cout << "{\"registered\":" << (code == 0 ? "true" : "false") << "}\n";
  } else {
    std::cout << (code == 0 ? "auto-restore task: registered\n" : "auto-restore task: not registered\n");
  }
  return 0;
}

/**
 * @brief Launch a program and move its top-level window onto a target rect.
 *
 * Windows opens new windows on the primary monitor by default. After the
 * process starts we poll for its visible top-level window and reposition it
 * (centered) inside the target rectangle, so the window appears on the
 * display the user was working on.
 *
 * @param commandLine Full command line to launch.
 * @param targetPos Top-left of the target monitor.
 * @param targetW Target monitor width.
 * @param targetH Target monitor height.
 * @return Process ID on success, or zero on failure.
 */
static DWORD LaunchAndMoveToRect(
  const std::wstring &commandLine,
  POINT targetPos,
  LONG targetW,
  LONG targetH
) {
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, const_cast<wchar_t *>(commandLine.c_str()),
                      nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                      nullptr, nullptr, &startup, &process)) {
    std::cerr << "CreateProcessW failed: " << GetLastError() << "\n";
    return 0;
  }
  CloseHandle(process.hThread);

  const DWORD processId = process.dwProcessId;
  for (int attempt = 0; attempt < 60; attempt++) {
    Sleep(200);
    struct MoveCtx { DWORD pid; const POINT *pos; LONG w, h; bool *found; };
    bool found = false;
    MoveCtx ctx = { processId, &targetPos, targetW, targetH, &found };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
      MoveCtx *c = reinterpret_cast<MoveCtx *>(lp);
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid != c->pid || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
      }
      WINDOWINFO info = {};
      info.cbSize = sizeof(info);
      if (!GetWindowInfo(hwnd, &info)) {
        return TRUE;
      }
      const int winW = info.rcWindow.right - info.rcWindow.left;
      const int winH = info.rcWindow.bottom - info.rcWindow.top;
      const int x = c->pos->x + (c->w - winW) / 2;
      const int y = c->pos->y + (c->h - winH) / 2;
      SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      *c->found = true;
      return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (found) {
      break;
    }
  }

  CloseHandle(process.hProcess);
  return processId;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  // Strip --json from the argument list so position-based commands like
  // "add <w> <h> [vsync]" never see it. The flag may appear anywhere.
  std::vector<char *> cleanArgs;
  cleanArgs.reserve(argc);
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) {
      g_JsonOutput = true;
      continue;
    }
    cleanArgs.push_back(argv[i]);
  }
  argc = static_cast<int>(cleanArgs.size());
  argv = cleanArgs.data();

  std::string cmd = argv[1];

  // Cross-session launch: iddctrl --session <id> <command> [args...]
  // Relaunch this executable inside the target session so GDI/DisplayConfig
  // operate on that session's desktop. A global --json flag is forwarded at
  // the end of the in-session command line.
  if (cmd == "--session") {
    if (argc < 4) {
      std::cerr << "Usage: iddctrl --session <id> <command> [args...]\n";
      return 1;
    }
    const DWORD sessionId = std::stoul(argv[2]);

    std::wstring inSessionArgs;
    for (int i = 3; i < argc; i++) {
      if (!inSessionArgs.empty()) {
        inSessionArgs += L" ";
      }
      inSessionArgs += Utf8ToWide(argv[i]);
    }
    if (g_JsonOutput) {
      inSessionArgs += L" --json";
    }

    std::cout << "Launching into session " << sessionId << ": "
              << argv[3] << "\n";
    return RunInSession(sessionId, inSessionArgs.c_str());
  }

  // iddctrl run <exe> [args...]: launch an arbitrary program (intended to be
  // used through --session so it runs on the target session's desktop).
  if (cmd == "run") {
    if (argc < 3) {
      std::cerr << "Usage: iddctrl run <exe> [args...]\n";
      return 1;
    }
    std::wstring commandLine;
    for (int i = 2; i < argc; i++) {
      if (!commandLine.empty()) {
        commandLine += L" ";
      }
      commandLine += Utf8ToWide(argv[i]);
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
      std::cerr << "CreateProcessW failed: " << GetLastError() << "\n";
      return 1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
  }

  // iddctrl run-display <index> <exe> [args...]: launch a program and move its
  // main window onto the specified virtual display. Windows opens new windows
  // on the primary by default; this relocates them so they appear directly on
  // the target monitor (e.g. a virtual display used for streaming).
  if (cmd == "run-display") {
    if (argc < 4) {
      std::cerr << "Usage: iddctrl run-display <index> <exe> [args...]\n";
      return 1;
    }
    const UINT32 displayIndex = static_cast<UINT32>(std::stoul(argv[2]));

    std::vector<std::wstring> vNames;
    std::vector<POINT> vPositions;
    std::vector<LONG> vWidths;
    std::vector<LONG> vHeights;
    const UINT32 vCount = EnumerateVirtualDisplays(&vNames, &vPositions, &vWidths, &vHeights);
    if (displayIndex == 0 || displayIndex > vCount) {
      std::cerr << "Virtual display " << displayIndex << " not found (active: " << vCount << ")\n";
      return 1;
    }
    const POINT targetPos = vPositions[displayIndex - 1];
    const LONG targetW = vWidths[displayIndex - 1];
    const LONG targetH = vHeights[displayIndex - 1];

    std::wstring commandLine;
    for (int i = 3; i < argc; i++) {
      if (!commandLine.empty()) {
        commandLine += L" ";
      }
      commandLine += Utf8ToWide(argv[i]);
    }

    const DWORD pid = LaunchAndMoveToRect(commandLine, targetPos, targetW, targetH);
    if (pid == 0) {
      return 1;
    }
    if (g_JsonOutput) {
      std::cout << "{\"pid\":" << pid
                << ",\"display\":" << displayIndex
                << ",\"x\":" << targetPos.x
                << ",\"y\":" << targetPos.y << "}\n";
    } else {
      std::cout << "Launched pid=" << pid << " on display " << displayIndex << "\n";
    }
    return 0;
  }

  // iddctrl run-here <exe> [args...]: launch a program and place its window on
  // the display the mouse cursor is currently on. This matches the expected
  // behavior of "open where I am": a program launched while working on a
  // secondary/virtual screen appears on that screen instead of the primary.
  if (cmd == "run-here") {
    if (argc < 3) {
      std::cerr << "Usage: iddctrl run-here <exe> [args...]\n";
      return 1;
    }
    POINT cursor = {};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) {
      std::cerr << "Failed to resolve monitor under cursor\n";
      return 1;
    }
    const POINT targetPos = { mi.rcMonitor.left, mi.rcMonitor.top };
    const LONG targetW = mi.rcMonitor.right - mi.rcMonitor.left;
    const LONG targetH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    std::wstring commandLine;
    for (int i = 2; i < argc; i++) {
      if (!commandLine.empty()) {
        commandLine += L" ";
      }
      commandLine += Utf8ToWide(argv[i]);
    }

    const DWORD pid = LaunchAndMoveToRect(commandLine, targetPos, targetW, targetH);
    if (pid == 0) {
      return 1;
    }
    if (g_JsonOutput) {
      std::cout << "{\"pid\":" << pid
                << ",\"x\":" << targetPos.x
                << ",\"y\":" << targetPos.y
                << ",\"w\":" << targetW
                << ",\"h\":" << targetH << "}\n";
    } else {
      std::cout << "Launched pid=" << pid << " on the monitor under the cursor "
                << "(" << targetW << "x" << targetH << " at " << targetPos.x << "," << targetPos.y << ")\n";
    }
    return 0;
  }

  if (cmd == "install") {
    return CmdInstall(argc, argv);
  } else if (cmd == "uninstall") {
    return CmdUninstall();
  } else if (cmd == "add" && argc >= 4) {
    return CmdAdd(argc, argv);
  } else if (cmd == "update" && argc >= 5) {
    return CmdUpdate(argc, argv);
  } else if (cmd == "remove" && argc >= 3) {
    return CmdRemove(argc, argv);
  } else if (cmd == "clear") {
    return CmdClear();
  } else if (cmd == "list") {
    return CmdList();
  } else if (cmd == "caps") {
    return CmdCaps();
  } else if (cmd == "render-list") {
    return CmdRenderList();
  } else if (cmd == "render-get") {
    return CmdRenderGet();
  } else if (cmd == "render-set") {
    return CmdRenderSet(argc, argv);
  } else if (cmd == "paths") {
    return CmdPaths();
  } else if (cmd == "displays") {
    return CmdDisplays();
  } else if (cmd == "displayconfig") {
    return CmdDisplayConfig();
  } else if (cmd == "advancedcolor") {
    const std::string action = argc >= 3 ? argv[2] : "status";
    if (action != "status" && action != "on" && action != "off") {
      std::cerr << "Usage: iddctrl advancedcolor [status|on|off] [index]\n";
      return 1;
    }
    const UINT32 hdrIndex = argc >= 4 ? static_cast<UINT32>(std::stoul(argv[3])) : 1;
    return CmdAdvancedColor(action, hdrIndex);
  } else if (cmd == "advdbg") {
    return CmdAdvColorDebug();
  } else if (cmd == "primary" && argc >= 3) {
    return CmdPrimary(static_cast<UINT32>(std::stoul(argv[2])));
  } else if (cmd == "physical-primary") {
    return CmdPhysicalPrimary();
  } else if (cmd == "layout") {
    std::vector<std::string> layoutArgs;
    for (int i = 2; i < argc; i++) {
      layoutArgs.push_back(argv[i]);
    }
    return CmdLayout(layoutArgs);
  } else if (cmd == "dxgicap") {
    return CmdDxgiCapture(argc, argv);
  } else if (cmd == "save-config") {
    const std::wstring file = argc >= 3 ? ToWideString(argv[2]) : std::wstring();
    return CmdSaveConfig(file);
  } else if (cmd == "restore") {
    const std::wstring file = argc >= 3 ? ToWideString(argv[2]) : std::wstring();
    return CmdRestore(file);
  } else if (cmd == "register-task") {
    const bool enable = argc < 3 || std::string(argv[2]) != "off";
    return CmdRegisterTask(enable);
  } else if (cmd == "task-status") {
    return CmdTaskStatus();
  } else {
    PrintUsage();
    return 1;
  }
}
