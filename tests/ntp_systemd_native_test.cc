// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Native D-Bus service test; accepts only a named disposable unit. */
#include <iostream>

#include "plugins/ntp/src/linux/chrony_service.h"
int main(int argc, char** argv) {
  if (argc == 3 &&
      std::string_view(argv[1]) == "dangd-ntp-native-fail-test.service" &&
      std::string_view(argv[2]) == "expect-failure") {
    dang::ntp::SystemdChronyService service(argv[1]);
    std::string error;
    if (service.SetActive(true, &error) || error.empty()) return 1;
    std::cout << "rejected failed job: " << error << '\n';
    bool active = true;
    if (!service.Active(&active, &error) || active) return 1;
    return service.SetActive(false, &error) ? 0 : 1;
  }
  if (argc != 3 ||
      std::string_view(argv[1]) != "dangd-ntp-native-test.service" ||
      (std::string_view(argv[2]) != "start" &&
       std::string_view(argv[2]) != "stop" &&
       std::string_view(argv[2]) != "reload"))
    return 2;
  dang::ntp::SystemdChronyService service(argv[1]);
  std::string error;
  const bool expected = std::string_view(argv[2]) != "stop";
  const bool success =
      std::string_view(argv[2]) == "reload"
          ? dang::native::ChangeSystemdUnit(
                argv[1], dang::native::UnitOperation::kReloadOrRestart, &error)
          : service.SetActive(expected, &error);
  if (!success) {
    std::cerr << error << '\n';
    return 1;
  }
  bool active = false;
  if (!service.Active(&active, &error) || active != expected) {
    std::cerr << "observed service state did not match: " << error << '\n';
    return 1;
  }
  std::cout << "acknowledged and observed " << (active ? "active" : "inactive")
            << '\n';
  return 0;
}
