// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Dedicated chrony command and descriptor-based ownership preflight. */
#include "plugins/ntp/src/chrony_deployment.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
namespace dang::ntp {
namespace {
bool Fail(const char* message, std::string* error) {
  if (error) *error = message;
  return false;
}
struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) close(fd);
  }
};
int Directory(const std::string& path, uid_t owner, std::string* error) {
  const std::filesystem::path parsed(path);
  if (!parsed.is_absolute() || parsed.lexically_normal() != parsed) {
    Fail("chrony path must be absolute and normalized", error);
    return -1;
  }
  Fd current{open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  std::string prefix;
  for (const auto& part : parsed.relative_path()) {
    prefix += "/" + part.string();
    Fd next{openat(current.fd, part.c_str(),
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat state{};
    if (next.fd < 0 || fstat(next.fd, &state) < 0 ||
        (state.st_uid != 0 && state.st_uid != owner)) {
      Fail("chrony directory is missing, symbolic or has unexpected ownership",
           error);
      return -1;
    }
    const bool temporary = (prefix == "/tmp" || prefix == "/var/tmp" ||
                            prefix == "/private/tmp") &&
                           prefix != path && state.st_uid == 0 &&
                           (state.st_mode & S_ISVTX);
    if ((state.st_mode & 0022) && !temporary) {
      Fail("chrony directory is writable by group or others", error);
      return -1;
    }
    close(current.fd);
    current.fd = next.fd;
    next.fd = -1;
  }
  const int result = current.fd;
  current.fd = -1;
  return result;
}
bool File(int directory, const char* name, uid_t owner, bool optional,
          bool private_key, bool executable, std::string* contents,
          std::string* error) {
  Fd file{
      openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (file.fd < 0)
    return (optional && errno == ENOENT) ||
           Fail("cannot securely open chrony file", error);
  struct stat state{};
  if (fstat(file.fd, &state) < 0 || !S_ISREG(state.st_mode) ||
      state.st_nlink != 1 || state.st_uid != owner ||
      (state.st_mode & (private_key ? 0077 : 0022)) ||
      (state.st_mode & 07000) || (executable && !(state.st_mode & 0100)))
    return Fail("chrony file has unsafe type, links, ownership or permissions",
                error);
  if (!contents) return true;
  contents->clear();
  char buffer[4096];
  for (;;) {
    const auto count = read(file.fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return Fail("cannot read chrony wrapper", error);
    if (count == 0) return true;
    contents->append(buffer, static_cast<std::size_t>(count));
    if (contents->size() > 1024U * 1024U) {
      contents->clear();
      return Fail("chrony wrapper exceeds 1 MiB", error);
    }
  }
}
}  // namespace
bool ValidateChronyInvocation(const std::string& executable,
                              const std::vector<std::string>& arguments,
                              const std::string& expected_binary,
                              const std::string& wrapper, bool isolated,
                              std::string* error) {
  std::vector<std::string> expected{expected_binary, "-n"};
  if (isolated) {
    expected.push_back("-x");
    expected.push_back("-u");
    expected.push_back("root");
  }
  expected.push_back("-f");
  expected.push_back(wrapper);
  return (executable == expected_binary && arguments == expected) ||
         Fail(
             "chrony service command does not match the dedicated executable "
             "and wrapper",
             error);
}
bool ValidateChronyTree(const std::string& root, uid_t owner,
                        std::string* wrapper, std::string* error) {
  if (!wrapper) return Fail("missing chrony wrapper output", error);
  wrapper->clear();
  Fd directory{Directory(root, owner, error)};
  if (directory.fd < 0) return false;
  struct stat state{};
  if (fstat(directory.fd, &state) < 0 || state.st_uid != owner)
    return Fail("chrony root has unexpected ownership", error);
  return File(directory.fd, "main.conf", owner, false, false, false, wrapper,
              error) &&
         File(directory.fd, "managed.conf", owner, true, false, false, nullptr,
              error) &&
         File(directory.fd, "managed.keys", owner, true, true, false, nullptr,
              error);
}
bool ValidateChronyBinary(const std::string& path, uid_t owner,
                          std::string* error) {
  const std::filesystem::path parsed(path);
  if (!parsed.is_absolute() || parsed.lexically_normal() != parsed)
    return Fail("chrony executable path must be absolute and normalized",
                error);
  Fd directory{Directory(parsed.parent_path().string(), owner, error)};
  return directory.fd >= 0 && File(directory.fd, parsed.filename().c_str(),
                                   owner, false, false, true, nullptr, error);
}
}  // namespace dang::ntp
