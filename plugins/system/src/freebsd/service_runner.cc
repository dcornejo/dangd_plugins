// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/** @file Fixed-argv access to FreeBSD's documented rc.d service boundary. */

#include "plugins/system/src/freebsd/service_runner.h"

#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstring>
#include <sstream>

extern char** environ;

namespace dang::system {

bool InterpretFreeBsdServiceStatus(int status, std::string* error) {
  if (!error) return false;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
  std::ostringstream message;
  message << "FreeBSD ntpd service command";
  if (WIFEXITED(status))
    message << " exited with status " << WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    message << " terminated by signal " << WTERMSIG(status);
  else
    message << " returned an unrecognized wait status";
  *error = message.str();
  return false;
}

bool RunFreeBsdNtpService(bool enabled, std::string* error) {
  if (!error) return false;
  char executable[] = "/usr/sbin/service";
  char service[] = "ntpd";
  char restart[] = "onerestart";
  char stop[] = "onestop";
  char* arguments[] = {executable, service, enabled ? restart : stop, nullptr};
  pid_t child = -1;
  const int spawned =
      posix_spawn(&child, executable, nullptr, nullptr, arguments, environ);
  if (spawned != 0) {
    *error = "cannot start FreeBSD service(8): " +
             std::string(std::strerror(spawned));
    return false;
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno == EINTR) continue;
    *error = "cannot wait for FreeBSD service(8): " +
             std::string(std::strerror(errno));
    return false;
  }
  return InterpretFreeBsdServiceStatus(status, error);
}

}  // namespace dang::system
