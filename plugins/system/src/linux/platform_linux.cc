// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Linux file layout and native systemd integration. */

#include <signal.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "plugins/system/src/platform.h"

namespace dang::system {
namespace {

constexpr std::string_view kSystemdService = "org.freedesktop.systemd1";
constexpr std::string_view kSystemdManagerPath = "/org/freedesktop/systemd1";
constexpr std::string_view kSystemdManager =
    "org.freedesktop.systemd1.Manager";
constexpr std::string_view kChronyUnit = "chrony.service";
constexpr auto kJobTimeout = std::chrono::seconds(30);

/** Completion state for the one systemd job returned by a manager method. */
struct JobResult {
  std::string path;
  std::string result;
  bool complete = false;
};

/** Records only the JobRemoved signal matching the returned job object path. */
int CaptureJobRemoved(sd_bus_message* message, void* user_data,
                      sd_bus_error*) {
  auto* state = static_cast<JobResult*>(user_data);
  std::uint32_t id = 0;
  const char* path = nullptr;
  const char* unit = nullptr;
  const char* result = nullptr;
  const int parsed =
      sd_bus_message_read(message, "uoss", &id, &path, &unit, &result);
  (void)id;
  (void)unit;
  if (parsed < 0 || !path || !result || state->path != path) return 0;
  state->result = result;
  state->complete = true;
  return 0;
}

/** Preserves a D-Bus diagnostic when present, otherwise the errno result. */
std::string BusError(std::string_view action, int result,
                     const sd_bus_error& bus_error) {
  std::string message(action);
  message += ": ";
  if (bus_error.message && *bus_error.message)
    message += bus_error.message;
  else
    message += std::strerror(-result);
  return message;
}

/** Enqueues one chrony unit method and waits for its exact asynchronous job. */
bool ChangeSystemdUnit(std::string_view method, std::string* error) {
  sd_bus* raw_bus = nullptr;
  int result = sd_bus_open_system(&raw_bus);
  if (result < 0) {
    *error = "cannot connect to the systemd system bus: " +
             std::string(std::strerror(-result));
    return false;
  }

  sd_bus_slot* match = nullptr;
  sd_bus_message* reply = nullptr;
  sd_bus_error bus_error = SD_BUS_ERROR_NULL;
  const auto cleanup = [&]() {
    sd_bus_error_free(&bus_error);
    sd_bus_message_unref(reply);
    sd_bus_slot_unref(match);
    sd_bus_flush_close_unref(raw_bus);
  };

  JobResult job;
  // Subscribe before invoking the manager method. systemd may complete a
  // short job immediately, so subscribing afterward would have a race between
  // receiving the returned object path and installing the signal match.
  result = sd_bus_add_match(
      raw_bus, &match,
      "type='signal',sender='org.freedesktop.systemd1',"
      "path='/org/freedesktop/systemd1',"
      "interface='org.freedesktop.systemd1.Manager',member='JobRemoved'",
      CaptureJobRemoved, &job);
  if (result < 0) {
    *error = "cannot subscribe to systemd job completion: " +
             std::string(std::strerror(-result));
    cleanup();
    return false;
  }
  result = sd_bus_call_method(
      raw_bus, kSystemdService.data(), kSystemdManagerPath.data(),
      kSystemdManager.data(), "Subscribe", &bus_error, nullptr, nullptr);
  if (result < 0) {
    *error = BusError("cannot enable systemd job notifications", result,
                      bus_error);
    cleanup();
    return false;
  }

  result = sd_bus_call_method(
      raw_bus, kSystemdService.data(), kSystemdManagerPath.data(),
      kSystemdManager.data(), method.data(), &bus_error, &reply, "ss",
      kChronyUnit.data(), "replace");
  if (result < 0) {
    *error = BusError("cannot enqueue systemd NTP service operation", result,
                      bus_error);
    cleanup();
    return false;
  }
  const char* job_path = nullptr;
  result = sd_bus_message_read(reply, "o", &job_path);
  if (result < 0 || !job_path) {
    *error = "systemd returned an invalid NTP service job path";
    cleanup();
    return false;
  }
  job.path = job_path;

  const auto deadline = std::chrono::steady_clock::now() + kJobTimeout;
  while (!job.complete) {
    result = sd_bus_process(raw_bus, nullptr);
    if (result < 0) {
      *error = "cannot process systemd NTP job result: " +
               std::string(std::strerror(-result));
      cleanup();
      return false;
    }
    if (result > 0) continue;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      *error = "timed out waiting for systemd NTP service job";
      cleanup();
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - now);
    result = sd_bus_wait(raw_bus,
                         static_cast<std::uint64_t>(remaining.count()));
    if (result < 0) {
      *error = "cannot wait for systemd NTP job result: " +
               std::string(std::strerror(-result));
      cleanup();
      return false;
    }
  }
  if (job.result != "done") {
    *error = "systemd NTP service job completed with result " + job.result;
    cleanup();
    return false;
  }
  cleanup();
  return true;
}

}  // namespace

PlatformLayout NativePlatformLayout() {
  return {
      .ntp_configuration = "/etc/chrony/conf.d/dangd.conf",
      .hostname_configuration = "/etc/hostname"};
}

bool NativeNtpServiceOperation(bool enabled, std::string* error) {
  if (!error) return false;
  return ChangeSystemdUnit(enabled ? "ReloadOrRestartUnit" : "StopUnit",
                           error);
}

bool NativePowerOperation(bool restart, std::string* error) {
  // systemd documents SIGRTMIN+4 as an orderly power-off request and
  // SIGRTMIN+5 as an orderly reboot request. Unlike reboot(2), these requests
  // let PID 1 stop services and unmount or synchronize filesystems first.
  const int signal = SIGRTMIN + (restart ? 5 : 4);
  if (::kill(1, signal) == 0) return true;
  if (error)
    *error = "Linux systemd power request failed: " +
             std::string(std::strerror(errno));
  return false;
}

}  // namespace dang::system
