// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Shared bounded systemd job correlation and observed-state readback. */
#include "plugins/common/systemd_unit.h"

#include <systemd/sd-bus.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
namespace dang::native {
namespace {
constexpr const char* kService = "org.freedesktop.systemd1";
constexpr const char* kPath = "/org/freedesktop/systemd1";
constexpr const char* kManager = "org.freedesktop.systemd1.Manager";
struct Bus {
  sd_bus* bus = nullptr;
  sd_bus_slot* slot = nullptr;
  sd_bus_message* reply = nullptr;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  ~Bus() {
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_slot_unref(slot);
    sd_bus_flush_close_unref(bus);
  }
};
bool Fail(const std::string& message, std::string* error) {
  if (error) *error = message;
  return false;
}
bool BusFailure(const char* action, int result, const Bus& bus,
                std::string* error) {
  return Fail(
      std::string(action) + ": " +
          (bus.error.message ? bus.error.message : std::strerror(-result)),
      error);
}
bool Open(Bus* bus, std::string* error) {
  int result = sd_bus_open_system(&bus->bus);
  if (result >= 0) result = sd_bus_set_method_call_timeout(bus->bus, 5000000);
  return result >= 0 || BusFailure("open systemd bus", result, *bus, error);
}
bool ReadState(Bus* bus, const std::string& unit, std::string* state,
               std::string* error) {
  sd_bus_message_unref(bus->reply);
  bus->reply = nullptr;
  int result =
      sd_bus_call_method(bus->bus, kService, kPath, kManager, "LoadUnit",
                         &bus->error, &bus->reply, "s", unit.c_str());
  if (result < 0)
    return BusFailure("resolve systemd unit", result, *bus, error);
  const char* path = nullptr;
  if (sd_bus_message_read(bus->reply, "o", &path) < 0 || !path)
    return Fail("invalid systemd unit object path", error);
  // Stopped units may have been garbage-collected after JobRemoved. Load
  // their definition again and pin it to this connection while reading state.
  result = sd_bus_call_method(bus->bus, kService, kPath, kManager, "RefUnit",
                              &bus->error, nullptr, "s", unit.c_str());
  if (result < 0) return BusFailure("retain systemd unit", result, *bus, error);
  char* value = nullptr;
  result = sd_bus_get_property_string(bus->bus, kService, path,
                                      "org.freedesktop.systemd1.Unit",
                                      "ActiveState", &bus->error, &value);
  if (result < 0)
    return BusFailure("read systemd ActiveState", result, *bus, error);
  *state = value ? value : "";
  free(value);
  return true;
}
// Retain early signals dispatched while the method reply is pending. Filter
// the unit here, then correlate the returned job path; never accept other jobs.
struct Jobs {
  std::string unit;
  std::map<std::string, std::string> results;
};
int Completed(sd_bus_message* message, void* opaque, sd_bus_error*) {
  auto& jobs = *static_cast<Jobs*>(opaque);
  std::uint32_t id = 0;
  const char *path = nullptr, *unit = nullptr, *result = nullptr;
  if (sd_bus_message_read(message, "uoss", &id, &path, &unit, &result) >= 0 &&
      path && unit && result && jobs.unit == unit && jobs.results.size() < 128)
    jobs.results[path] = result;
  return 0;
}
}  // namespace
bool SystemdUnitActive(std::string_view unit, bool* active,
                       std::string* error) {
  if (!active) return Fail("missing service-state output", error);
  Bus bus;
  if (!Open(&bus, error)) return false;
  std::string state;
  if (!ReadState(&bus, std::string(unit), &state, error)) return false;
  if (state != "active" && state != "inactive" && state != "failed")
    return Fail("systemd unit is in transitional state " + state, error);
  *active = state == "active";
  return true;
}
bool ChangeSystemdUnit(std::string_view unit, UnitOperation operation,
                       std::string* error) {
  const std::string name(unit);
  Bus bus;
  if (!Open(&bus, error)) return false;
  Jobs jobs{.unit = name, .results = {}};
  int result = sd_bus_add_match(
      bus.bus, &bus.slot,
      "type='signal',sender='org.freedesktop.systemd1',path='/org/freedesktop/"
      "systemd1',"
      "interface='org.freedesktop.systemd1.Manager',member='JobRemoved'",
      Completed, &jobs);
  if (result < 0)
    return BusFailure("subscribe to systemd jobs", result, bus, error);
  result = sd_bus_call_method(bus.bus, kService, kPath, kManager, "Subscribe",
                              &bus.error, nullptr, nullptr);
  if (result < 0)
    return BusFailure("enable systemd job notifications", result, bus, error);
  const char* method = operation == UnitOperation::kStop ? "StopUnit"
                       : operation == UnitOperation::kStart
                           ? "StartUnit"
                           : "ReloadOrRestartUnit";
  result =
      sd_bus_call_method(bus.bus, kService, kPath, kManager, method, &bus.error,
                         &bus.reply, "ss", name.c_str(), "replace");
  if (result < 0)
    return BusFailure("enqueue systemd operation", result, bus, error);
  const char* path = nullptr;
  if (sd_bus_message_read(bus.reply, "o", &path) < 0 || !path)
    return Fail("invalid systemd job path", error);
  const std::string job(path);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!jobs.results.contains(job)) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return Fail("timed out waiting for systemd job", error);
    result = sd_bus_process(bus.bus, nullptr);
    if (result < 0)
      return BusFailure("process systemd job", result, bus, error);
    if (result > 0) continue;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    result =
        sd_bus_wait(bus.bus, static_cast<std::uint64_t>(remaining.count()));
    if (result < 0)
      return BusFailure("wait for systemd job", result, bus, error);
  }
  if (jobs.results[job] != "done")
    return Fail("systemd job result: " + jobs.results[job], error);
  std::string state;
  if (!ReadState(&bus, name, &state, error)) return false;
  const char* expected =
      operation == UnitOperation::kStop ? "inactive" : "active";
  // StopUnit can leave a previously failed unit in "failed" until
  // ResetFailedUnit. That is a stable non-running state, not a failed stop.
  return state == expected ||
         (operation == UnitOperation::kStop && state == "failed") ||
         Fail("systemd job completed but ActiveState is " + state, error);
}
bool SystemdExecStart(std::string_view unit, std::vector<UnitCommand>* commands,
                      std::string* error) {
  if (!commands) return Fail("missing systemd command output", error);
  commands->clear();
  Bus bus;
  if (!Open(&bus, error)) return false;
  std::string state;
  if (!ReadState(&bus, std::string(unit), &state, error)) return false;
  sd_bus_message_rewind(bus.reply, true);
  const char* raw_path = nullptr;
  if (sd_bus_message_read(bus.reply, "o", &raw_path) < 0 || !raw_path)
    return Fail("invalid systemd service object path", error);
  const std::string path(raw_path);
  sd_bus_message_unref(bus.reply);
  bus.reply = nullptr;
  const int result = sd_bus_call_method(
      bus.bus, kService, path.c_str(), "org.freedesktop.DBus.Properties", "Get",
      &bus.error, &bus.reply, "ss", "org.freedesktop.systemd1.Service",
      "ExecStart");
  if (result < 0)
    return BusFailure("read systemd ExecStart", result, bus, error);
  if (sd_bus_message_enter_container(bus.reply, 'v', "a(sasbttttuii)") <= 0 ||
      sd_bus_message_enter_container(bus.reply, 'a', "(sasbttttuii)") <= 0)
    return Fail("invalid systemd ExecStart signature", error);
  std::vector<UnitCommand> parsed;
  int entry;
  while ((entry = sd_bus_message_enter_container(bus.reply, 'r',
                                                 "sasbttttuii")) > 0) {
    UnitCommand command;
    const char* value = nullptr;
    if (sd_bus_message_read(bus.reply, "s", &value) <= 0 || !value ||
        sd_bus_message_enter_container(bus.reply, 'a', "s") <= 0)
      return Fail("invalid systemd command entry", error);
    command.executable = value;
    int arg;
    while ((arg = sd_bus_message_read(bus.reply, "s", &value)) > 0) {
      command.arguments.emplace_back(value ? value : "");
      if (command.arguments.size() > 128)
        return Fail("systemd command has too many arguments", error);
    }
    int ignore = 0;
    if (arg < 0 || sd_bus_message_exit_container(bus.reply) < 0 ||
        sd_bus_message_read(bus.reply, "b", &ignore) <= 0 ||
        sd_bus_message_skip(bus.reply, "ttttuii") < 0 ||
        sd_bus_message_exit_container(bus.reply) < 0)
      return Fail("invalid systemd command metadata", error);
    command.ignore_failure = ignore != 0;
    parsed.push_back(std::move(command));
    if (parsed.size() > 16)
      return Fail("too many systemd start commands", error);
  }
  if (entry < 0) return Fail("invalid systemd command array", error);
  *commands = std::move(parsed);
  return true;
}
bool SystemdUnitPolicy(std::string_view unit, UnitPolicy* policy,
                       std::string* error) {
  if (!policy) return Fail("missing systemd policy output", error);
  *policy = {};
  Bus bus;
  std::string state;
  if (!Open(&bus, error) || !ReadState(&bus, std::string(unit), &state, error))
    return false;
  sd_bus_message_rewind(bus.reply, true);
  const char* raw_path = nullptr;
  if (sd_bus_message_read(bus.reply, "o", &raw_path) <= 0 || !raw_path)
    return Fail("invalid unit policy object path", error);
  const std::string path(raw_path);
  const auto get = [&](const char* interface, const char* name,
                       const char* signature) {
    sd_bus_message_unref(bus.reply);
    bus.reply = nullptr;
    const int result = sd_bus_call_method(
        bus.bus, kService, path.c_str(), "org.freedesktop.DBus.Properties",
        "Get", &bus.error, &bus.reply, "ss", interface, name);
    if (result < 0)
      return BusFailure("read systemd policy property", result, bus, error);
    return sd_bus_message_enter_container(bus.reply, 'v', signature) > 0 ||
           Fail("invalid systemd policy property signature", error);
  };
  constexpr const char* service = "org.freedesktop.systemd1.Service";
  constexpr const char* unit_interface = "org.freedesktop.systemd1.Unit";
  const auto string = [&](const char* interface, const char* name,
                          std::string* output) {
    const char* value = nullptr;
    if (!get(interface, name, "s") ||
        sd_bus_message_read(bus.reply, "s", &value) <= 0 || !value)
      return false;
    *output = value;
    return true;
  };
  const auto nonempty = [&](const char* interface, const char* name,
                            const char* signature, const char* element,
                            bool* output) {
    if (!get(interface, name, signature) ||
        sd_bus_message_enter_container(bus.reply, 'a', element) <= 0)
      return false;
    const int empty = sd_bus_message_at_end(bus.reply, false);
    if (empty < 0) return false;
    *output = *output || empty == 0;
    return true;
  };
  UnitPolicy result;
  if (!string(unit_interface, "FragmentPath", &result.fragment_path) ||
      !string(service, "Type", &result.type) ||
      !string(service, "User", &result.user) ||
      !string(service, "Group", &result.group) ||
      !string(service, "Restart", &result.restart) ||
      !nonempty(unit_interface, "DropInPaths", "as", "s", &result.drop_ins))
    return false;
  for (const char* name : {"ExecCondition", "ExecStartPre", "ExecStartPost",
                           "ExecReload", "ExecStop", "ExecStopPost"})
    if (!nonempty(service, name, "a(sasbttttuii)", "(sasbttttuii)",
                  &result.auxiliary_commands))
      return false;
  for (const char* name :
       {"Environment", "PassEnvironment", "UnsetEnvironment"})
    if (!nonempty(service, name, "as", "s", &result.environment)) return false;
  if (!nonempty(service, "EnvironmentFiles", "a(sb)", "(sb)",
                &result.environment))
    return false;
  for (const char* name : {"RootDirectory", "RootImage", "WorkingDirectory"}) {
    std::string value;
    if (!string(service, name, &value)) return false;
    result.filesystem_override = result.filesystem_override || !value.empty();
  }
  for (const char* name : {"BindPaths", "BindReadOnlyPaths"})
    if (!nonempty(service, name, "a(ssbt)", "(ssbt)",
                  &result.filesystem_override))
      return false;
  if (!nonempty(service, "TemporaryFileSystem", "a(ss)", "(ss)",
                &result.filesystem_override))
    return false;
  int no_new_privileges = 0;
  if (!get(service, "NoNewPrivileges", "b") ||
      sd_bus_message_read(bus.reply, "b", &no_new_privileges) <= 0 ||
      !get(service, "CapabilityBoundingSet", "t") ||
      sd_bus_message_read(bus.reply, "t", &result.capability_bounding_set) <=
          0 ||
      !get(service, "MainPID", "u") ||
      sd_bus_message_read(bus.reply, "u", &result.main_pid) <= 0)
    return false;
  result.no_new_privileges = no_new_privileges != 0;
  // Extended command flags can alter privilege/environment semantics even
  // when the legacy executable and argv fields appear identical.
  if (!get(service, "ExecStartEx", "a(sasasttttuii)") ||
      sd_bus_message_enter_container(bus.reply, 'a', "(sasasttttuii)") <= 0)
    return false;
  int command;
  while ((command = sd_bus_message_enter_container(bus.reply, 'r',
                                                   "sasasttttuii")) > 0) {
    if (sd_bus_message_skip(bus.reply, "sas") < 0 ||
        sd_bus_message_enter_container(bus.reply, 'a', "s") <= 0)
      return false;
    const int empty = sd_bus_message_at_end(bus.reply, false);
    if (empty < 0) return false;
    result.privileged_start_flags = result.privileged_start_flags || empty == 0;
    // Finish any flag array before reading timing metadata.
    const char* flag = nullptr;
    while (sd_bus_message_read(bus.reply, "s", &flag) > 0) {
    }
    if (sd_bus_message_exit_container(bus.reply) < 0 ||
        sd_bus_message_skip(bus.reply, "ttttuii") < 0 ||
        sd_bus_message_exit_container(bus.reply) < 0)
      return false;
  }
  if (command < 0) return false;
  *policy = std::move(result);
  return true;
}
}  // namespace dang::native
