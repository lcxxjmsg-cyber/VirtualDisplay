#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

#define VD_IDD_LOG_PATH "C:\\ProgramData\\VirtualDisplay\\VirtualDisplay.log"
#define VD_IDD_FALLBACK_LOG_PATH "C:\\Windows\\Temp\\VirtualDisplay.log"

/**
 * @brief Ensure the VirtualDisplay IddCx diagnostic log directory exists.
 */
inline void
VdIddEnsureLogDirectory()
{
  CreateDirectoryA("C:\\ProgramData\\VirtualDisplay", nullptr);
}

/**
 * @brief Open the VirtualDisplay IddCx diagnostic log file.
 *
 * @return Writable append-only file handle, or `INVALID_HANDLE_VALUE` when no log path is writable.
 */
inline HANDLE
VdIddOpenLogFile()
{
  VdIddEnsureLogDirectory();

  HANDLE file = CreateFileA(
    VD_IDD_LOG_PATH,
    FILE_APPEND_DATA,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    return file;
  }

  return CreateFileA(
    VD_IDD_FALLBACK_LOG_PATH,
    FILE_APPEND_DATA,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
}

/**
 * @brief Append an ASCII diagnostic line to the VirtualDisplay IddCx log file.
 *
 * @param format `printf`-style format string.
 */
inline void
VdIddLog(
  _In_z_ _Printf_format_string_ const char *format,
  ...)
{
  HANDLE file = VdIddOpenLogFile();
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  SYSTEMTIME now = {};
  GetSystemTime(&now);

  char message[512] = {};
  va_list args;
  va_start(args, format);
  vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
  va_end(args);

  char line[768] = {};
  const int length = sprintf_s(
    line,
    "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s\r\n",
    now.wYear,
    now.wMonth,
    now.wDay,
    now.wHour,
    now.wMinute,
    now.wSecond,
    now.wMilliseconds,
    message);
  if (length > 0) {
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
  }

  CloseHandle(file);
}
