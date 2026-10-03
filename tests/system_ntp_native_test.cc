// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <iostream>
#include <string>
#include <string_view>

#include "plugins/system/src/platform.h"

int main(int argc, char** argv) {
  if (argc != 2 || (std::string_view(argv[1]) != "enable" &&
                    std::string_view(argv[1]) != "disable")) {
    std::cerr << "usage: system_ntp_native_test enable|disable\n";
    return 2;
  }
  std::string error;
  if (dang::system::NativeNtpServiceOperation(
          std::string_view(argv[1]) == "enable", &error))
    return 0;
  std::cerr << error << '\n';
  return 1;
}
