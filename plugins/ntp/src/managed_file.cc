// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Durable, reversible POSIX managed-file replacement for NTP adapters.
 */
#include "plugins/ntp/src/managed_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>

namespace dang::ntp {
namespace {
bool Fail(const char* operation, std::string* error) {
  if (error) *error = std::string(operation) + ": " + std::strerror(errno);
  return false;
}
bool Invalid(const char* message, std::string* error) {
  if (error) *error = message;
  return false;
}
void Erase(std::string& bytes) {
  volatile char* data = bytes.data();
  for (std::size_t i = 0; i < bytes.size(); ++i) data[i] = 0;
  bytes.clear();
}
}  // namespace

ManagedFile::~ManagedFile() {
  Erase(before_.bytes);
  Erase(installed_.bytes);
  if (directory_ >= 0) close(directory_);
}

bool ManagedFile::Capture(const std::string& path, std::string* error) {
  if (directory_ >= 0)
    return Invalid("managed-file capture may only run once", error);
  const std::filesystem::path parsed(path);
  if (!parsed.is_absolute() || parsed.filename().empty() ||
      parsed.filename() == "." || parsed.filename() == "..")
    return Invalid("managed file requires an absolute regular-file path",
                   error);
  name_ = parsed.filename().string();
  directory_ = open(parsed.parent_path().c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_ < 0) return Fail("open managed-file directory", error);
  captured_ = Read(&before_, error);
  return captured_;
}

bool ManagedFile::Read(Image* image, std::string* error) {
  const int fd = openat(directory_, name_.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT) {
      *image = {};
      return true;
    }
    return Fail("open managed file", error);
  }
  struct stat state{};
  if (fstat(fd, &state) < 0) {
    const int saved = errno;
    close(fd);
    errno = saved;
    return Fail("inspect managed file", error);
  }
  if (!S_ISREG(state.st_mode) || state.st_nlink != 1 || state.st_size < 0 ||
      state.st_size > 4 * 1024 * 1024) {
    close(fd);
    return Invalid(
        "managed file must be a singly linked regular file at most 4 MiB",
        error);
  }
  image->exists = true;
  image->mode = state.st_mode & 0777;
  image->owner = state.st_uid;
  image->group = state.st_gid;
  image->bytes.clear();
  char buffer[4096];
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      const int saved = errno;
      close(fd);
      errno = saved;
      return Fail("read managed file", error);
    }
    image->bytes.append(buffer, static_cast<std::size_t>(count));
    if (image->bytes.size() > 4U * 1024U * 1024U) {
      close(fd);
      return Invalid("managed file grew beyond the capture limit", error);
    }
  }
  close(fd);
  return true;
}

bool ManagedFile::Matches(const Image& expected, std::string* error) {
  Image actual;
  const bool read = Read(&actual, error);
  const bool same =
      read && actual.exists == expected.exists &&
      (!actual.exists ||
       (actual.bytes == expected.bytes && actual.mode == expected.mode &&
        actual.owner == expected.owner && actual.group == expected.group));
  Erase(actual.bytes);
  if (!read) return false;
  return same ||
         Invalid("managed file changed outside this transaction", error);
}

bool ManagedFile::Replace(const Image& image, std::string* error) {
  if (!image.exists) {
    if (unlinkat(directory_, name_.c_str(), 0) < 0 && errno != ENOENT)
      return Fail("remove managed file", error);
  } else {
    static std::atomic<unsigned long> sequence{0};
    const std::string temporary = ".dangd-ntp-" + std::to_string(getpid()) +
                                  "-" + std::to_string(sequence++);
    const int fd =
        openat(directory_, temporary.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return Fail("create managed-file temporary", error);
    bool ok = true;
    std::size_t offset = 0;
    while (offset < image.bytes.size()) {
      const ssize_t count =
          write(fd, image.bytes.data() + offset, image.bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {
        ok = false;
        break;
      }
      offset += static_cast<std::size_t>(count);
    }
    if (ok) ok = fchown(fd, image.owner, image.group) == 0;
    if (ok) ok = fchmod(fd, image.mode) == 0;
    if (ok) ok = (sync_ ? sync_(fd) : fsync(fd)) == 0;
    int saved = errno;
    if (close(fd) < 0 && ok) {
      ok = false;
      saved = errno;
    }
    if (ok && renameat(directory_, temporary.c_str(), directory_,
                       name_.c_str()) < 0) {
      ok = false;
      saved = errno;
    }
    if (!ok) {
      unlinkat(directory_, temporary.c_str(), 0);
      errno = saved;
      return Fail("replace managed file", error);
    }
  }
  // The visible mutation precedes fsync. Retain its image even on failure so
  // callers can compensate rather than treating a durability error as a no-op.
  Erase(installed_.bytes);
  installed_ = image;
  changed_ = true;
  if ((sync_ ? sync_(directory_) : fsync(directory_)) < 0)
    return Fail("sync managed-file directory", error);
  return true;
}

bool ManagedFile::Install(std::optional<std::string_view> contents, mode_t mode,
                          std::string* error) {
  if (!captured_)
    return Invalid("managed file has no captured before-image", error);
  if (!Matches(changed_ ? installed_ : before_, error)) return false;
  Image image;
  image.exists = contents.has_value();
  if (contents) image.bytes = *contents;
  image.mode = mode & 0777;
  image.owner = geteuid();
  image.group = getegid();
  const bool result = Replace(image, error);
  Erase(image.bytes);
  return result;
}

bool ManagedFile::Rollback(std::string* error) {
  if (!captured_)
    return Invalid("managed file has no captured before-image", error);
  if (!changed_) return true;
  if (!Matches(installed_, error)) return false;
  if (!Replace(before_, error)) return false;
  changed_ = false;
  return true;
}
}  // namespace dang::ntp
