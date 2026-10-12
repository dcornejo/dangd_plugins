// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Conservative exclusive-ownership policy for a chrony wrapper file. */
#include "plugins/ntp/src/chrony_preflight.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
namespace dang::ntp {
bool ValidateChronyWrapper(std::string_view text, std::string_view fragment,
                           bool isolated_test, std::string* error) {
  unsigned includes = 0;
  std::istringstream input{std::string(text)};
  std::string line;
  unsigned number = 0;
  while (std::getline(input, line)) {
    ++number;
    std::istringstream fields(line);
    std::string directive, value, extra;
    if (!(fields >> directive) || directive.starts_with('#') ||
        directive.starts_with('!') || directive.starts_with(';') ||
        directive.starts_with('%'))
      continue;
    std::ranges::transform(directive, directive.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    fields >> value;
    fields >> extra;
    const bool single = !value.empty() && extra.empty();
    const bool infrastructure =
        directive == "pidfile" || directive == "driftfile" ||
        directive == "logdir" || directive == "bindcmdaddress";
    const bool loopback = isolated_test && single && value == "127.0.0.1" &&
                          (directive == "bindaddress" || directive == "allow");
    if (directive == "include" && single && value == fragment &&
        value.find_first_of("*?[") == std::string::npos && ++includes == 1)
      continue;
    if (infrastructure && single && value.front() == '/') continue;
    if (directive == "cmdport" && single && value == "0") continue;
    if (loopback) continue;
    // Never echo full directive arguments: an unknown directive may carry
    // credentials. Attribute the line and leave details in the owner's file.
    if (error)
      *error = "unmanaged or unsupported chrony directive at line " +
               std::to_string(number);
    return false;
  }
  if (includes != 1) {
    if (error)
      *error = "chrony wrapper must include the managed fragment exactly once";
    return false;
  }
  return true;
}
}  // namespace dang::ntp
