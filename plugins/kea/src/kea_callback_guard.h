// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANG_PLUGINS_KEA_CALLBACK_GUARD_H_
#define DANG_PLUGINS_KEA_CALLBACK_GUARD_H_

#include <exception>
#include <string_view>
#include <utility>

namespace dang::plugins::kea {

/** Prevents C++ exceptions from crossing a C plugin callback boundary. */
template <typename Callback, typename Failure>
int GuardPluginCallback(Callback&& callback, Failure&& failure) noexcept {
  try {
    return std::forward<Callback>(callback)();
  } catch (const std::exception& exception) {
    std::forward<Failure>(failure)(std::string_view(exception.what()));
  } catch (...) {
    std::forward<Failure>(failure)(std::string_view("unknown exception"));
  }
  return 0;
}

}  // namespace dang::plugins::kea

#endif  // DANG_PLUGINS_KEA_CALLBACK_GUARD_H_
