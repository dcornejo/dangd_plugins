// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
#ifndef DANG_PLUGINS_NTP_MANAGED_FILE_H_
#define DANG_PLUGINS_NTP_MANAGED_FILE_H_

#include <sys/types.h>

#include <optional>
#include <string>
#include <string_view>

namespace dang::ntp {

/**
 * Reversible replacement of one regular file in a retained directory.
 *
 * Capture opens an existing trusted directory; no directory is created. The
 * final file must be absent or a singly linked regular file, never a symlink.
 * Replacements use exclusive temporary files, fsync, renameat and directory
 * fsync. A rename followed by a durability failure still requires rollback.
 * The owner/group and permission bits of an existing file are restored.
 *
 * This is a per-file primitive, not an atomic transaction across both chrony
 * files. The future adapter must stop chrony, replace keys before its include,
 * compensate both files on failure, and only then restart the service.
 */
class ManagedFile {
 public:
  ManagedFile() = default;
  /** Inject fsync only for deterministic durability-failure tests. */
  explicit ManagedFile(int (*sync)(int)) : sync_(sync) {}
  ~ManagedFile();
  ManagedFile(const ManagedFile&) = delete;
  ManagedFile& operator=(const ManagedFile&) = delete;

  /** Capture an absent or existing file before any replacement. */
  bool Capture(const std::string& absolute_path, std::string* error);
  /** Install bytes with a prescribed mode, or remove the managed file. */
  bool Install(std::optional<std::string_view> contents, mode_t mode,
               std::string* error);
  /** Restore the captured bytes and metadata; refuse unrelated later edits. */
  bool Rollback(std::string* error);
  /** True after a mutation, even when directory fsync failed afterwards. */
  bool changed() const { return changed_; }

 private:
  struct Image {
    bool exists = false;
    std::string bytes;
    mode_t mode = 0;
    uid_t owner = 0;
    gid_t group = 0;
  };
  bool Read(Image* image, std::string* error);
  bool Replace(const Image& image, std::string* error);
  bool Matches(const Image& image, std::string* error);
  int directory_ = -1;
  std::string name_;
  Image before_;
  Image installed_;
  bool captured_ = false;
  bool changed_ = false;
  int (*sync_)(int) = nullptr;
};

}  // namespace dang::ntp
#endif  // DANG_PLUGINS_NTP_MANAGED_FILE_H_
