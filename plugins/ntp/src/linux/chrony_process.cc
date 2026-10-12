// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Read-only, pidfd-pinned identity checks for the live chronyd process.
 */
#include "plugins/ntp/src/linux/chrony_process.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <sstream>
namespace dang::ntp {
namespace {
struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) close(fd);
  }
};
bool Fail(const char* text, std::string* error) {
  if (error) *error = text;
  return false;
}
bool Read(int directory, const char* name, std::string* contents) {
  Fd file{openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (file.fd < 0) return false;
  char buffer[4096];
  for (;;) {
    const auto count = read(file.fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return false;
    if (count == 0) return true;
    contents->append(buffer, static_cast<std::size_t>(count));
    if (contents->size() > 65536) return false;
  }
}
}  // namespace
bool ValidateChronyProcess(std::uint32_t pid, const std::string& executable,
                           const std::vector<std::string>& arguments,
                           uid_t owner, std::string* error) {
  if (!pid) return Fail("chrony service has no live main process", error);
  Fd process{static_cast<int>(syscall(SYS_pidfd_open, pid, 0))};
  Fd directory{open(("/proc/" + std::to_string(pid)).c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  // The procfs exe link is intentionally followed, unlike configured paths.
  Fd actual{directory.fd >= 0
                ? openat(directory.fd, "exe", O_RDONLY | O_CLOEXEC)
                : -1};
  Fd expected{open(executable.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  struct stat a{}, b{};
  if (process.fd < 0 || actual.fd < 0 || expected.fd < 0 ||
      fstat(actual.fd, &a) < 0 || fstat(expected.fd, &b) < 0 ||
      a.st_dev != b.st_dev || a.st_ino != b.st_ino)
    return Fail("chrony running executable does not match the trusted binary",
                error);
  std::string command, status;
  if (!Read(directory.fd, "cmdline", &command) ||
      !Read(directory.fd, "status", &status) || command.empty() ||
      command.back() != '\0')
    return Fail("cannot read stable chrony process identity", error);
  std::vector<std::string> observed;
  std::size_t start = 0;
  for (std::size_t end = 0; end < command.size(); ++end)
    if (command[end] == '\0') {
      observed.push_back(command.substr(start, end - start));
      start = end + 1;
    }
  if (observed != arguments)
    return Fail("chrony running argv does not match the dedicated invocation",
                error);
  bool matching_uid = false;
  std::istringstream lines(status);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.starts_with("Uid:")) continue;
    std::istringstream values(line.substr(4));
    unsigned long real, effective, saved, filesystem;
    matching_uid =
        static_cast<bool>(values >> real >> effective >> saved >> filesystem) &&
        real == owner && effective == owner && saved == owner &&
        filesystem == owner;
  }
  pollfd state{.fd = process.fd, .events = POLLIN, .revents = 0};
  if (!matching_uid || poll(&state, 1, 0) != 0)
    return Fail("chrony process exited or has unexpected credentials", error);
  return true;
}
}  // namespace dang::ntp
