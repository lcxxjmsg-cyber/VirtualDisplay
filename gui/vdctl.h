#pragma once

#include "json.h"
#include <string>
#include <vector>

namespace vdgui {

struct CtlResult {
  int exitCode = -1;
  std::string output;   // merged stdout+stderr
};

/**
 * @brief Locate iddctrl.exe next to this executable (or in PATH).
 *
 * @return Absolute path or bare name.
 */
std::wstring FindIddctrl();

/**
 * @brief Run iddctrl with --json appended and capture output.
 *
 * @param args Command arguments (without "--json").
 * @param timeoutMs Maximum wait time.
 * @return Exit code and captured output.
 */
CtlResult RunCtl(const std::vector<std::string> &args, int timeoutMs = 30000);

/**
 * @brief Convenience: run and parse output as JSON.
 */
struct CtlJson {
  bool ok = false;
  bool validJson = false;
  int exitCode = -1;
  std::string raw;
  JsonValue value;
};
CtlJson RunCtlJson(const std::vector<std::string> &args, int timeoutMs = 30000);

}  // namespace vdgui
