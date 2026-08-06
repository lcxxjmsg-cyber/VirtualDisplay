#include "vdctl.h"
#include "json.h"
#include "strutil.h"

#include <windows.h>

namespace vdgui {

std::wstring FindIddctrl() {
  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
  std::wstring dir(modulePath);
  auto slash = dir.find_last_of(L'\\');
  if (slash != std::wstring::npos) dir.resize(slash + 1);

  std::wstring candidate = dir + L"iddctrl.exe";
  if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return candidate;
  }
  return L"iddctrl.exe";
}

CtlResult RunCtl(const std::vector<std::string> &args, int timeoutMs) {
  CtlResult result;
  std::wstring command = L"\"" + FindIddctrl() + L"\"";
  for (const auto &a : args) {
    command += L" ";
    command += L"\"";
    command += Utf8ToWide(a);
    command += L"\"";
  }
  command += L" --json";

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
    return result;
  }
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi = {};
  std::wstring cmdline = command;  // writable buffer
  BOOL created = CreateProcessW(
    nullptr, cmdline.data(), nullptr, nullptr, TRUE,
    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(writePipe);

  if (!created) {
    CloseHandle(readPipe);
    return result;
  }

  // Read until EOF (process may still be running; close pipe after wait).
  // Enforce a hard wall-clock timeout so a wedged child (e.g. pnputil waiting
  // on a locked DriverStore) cannot hang the UI forever.
  std::string output;
  char buf[4096];
  DWORD read = 0;
  const ULONGLONG startedAt = GetTickCount64();
  for (;;) {
    // Read available data
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &read, nullptr) && read > 0) {
      if (!ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) || read == 0) break;
      output.append(buf, read);
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, 50);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_TIMEOUT) {
      if (GetTickCount64() - startedAt > static_cast<ULONGLONG>(timeoutMs)) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        output += "[timed out after ";
        output += std::to_string(timeoutMs);
        output += " ms, process terminated]\n";
        break;
      }
      continue;
    }
    break;
  }
  // Drain remaining
  for (;;) {
    if (!ReadFile(readPipe, buf, sizeof(buf), &read, nullptr) || read == 0) break;
    output.append(buf, read);
  }

  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(readPipe);

  result.exitCode = static_cast<int>(code);
  result.output = std::move(output);
  return result;
}

CtlJson RunCtlJson(const std::vector<std::string> &args, int timeoutMs) {
  CtlJson result;
  CtlResult r = RunCtl(args, timeoutMs);
  result.exitCode = r.exitCode;
  result.raw = std::move(r.output);
  result.ok = result.exitCode == 0;
  bool valid = false;
  result.value = JsonParse(result.raw, &valid);
  result.validJson = valid;
  return result;
}

}  // namespace vdgui
