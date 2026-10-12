// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0
/** @file Ordered chrony hardware actions sharing one compensation checkpoint.
 */
#include "plugins/ntp/src/chrony_action_adapter.h"

#include <utility>
namespace dang::ntp {
namespace {
class ActionAdapter final : public NativeAdapter {
 public:
  ActionAdapter(AdapterDescriptor descriptor,
                std::unique_ptr<ChronyService> service, std::string fragment,
                std::string keys)
      : fallback_(CreateFailClosedAdapter(std::move(descriptor))),
        service_(std::move(service)),
        transaction_(*service_),
        fragment_(std::move(fragment)),
        keys_(std::move(keys)) {}
  ~ActionAdapter() override {
    volatile char* bytes = image_.key_file.data();
    for (std::size_t i = 0; i < image_.key_file.size(); ++i) bytes[i] = 0;
  }
  const AdapterDescriptor& descriptor() const override {
    return fallback_->descriptor();
  }
  bool ValidateConfig(const Config& proposed, std::string* error,
                      std::string* path) override {
    ChronyConfigImage image;
    return RenderChronyConfig(proposed, keys_, &image, error, path);
  }
  bool ApplyAction(const Config& before, const Config& proposed,
                   const PlannedAction& action, AdapterCheckpoint* checkpoint,
                   std::string* error, std::string* path) override {
    if (path) *path = ActionPath(action.id);
    const auto plan = PlanChanges(before, proposed);
    if (!checkpoint || aborted_ || next_ >= plan.size() ||
        plan[next_] != action)
      return Fail(
          "chrony action is out of order or transaction is no longer "
          "applicable",
          error);
    checkpoint->action_id = action.id;
    checkpoint->applied = false;
    if (!captured_) {
      if (!RenderChronyConfig(proposed, keys_, &image_, error, path) ||
          !transaction_.Capture(fragment_, keys_, error))
        return false;
      captured_ = true;
    }
    bool ok = true;
    if (!stopped_) {
      ok = transaction_.Stop(error);
      stopped_ = ok;
    }
    const bool activation = action.id == "enable-ntp" ||
                            action.id == "restart-ntp-after-reconfiguration";
    // Staging the complete proposed image once keeps key/fragment references
    // coherent. Remaining logical deltas acknowledge that shared file stage.
    if (ok && !installed_ && action.id != "stop-ntp-for-reconfiguration") {
      ok = transaction_.Install(image_, error);
      installed_ = ok;
    }
    if (ok && activation) ok = transaction_.Activate(error);
    if (!ok) {
      aborted_ = true;
      return false;
    }
    checkpoint->before_image = "retained shared chrony transaction";
    checkpoint->applied = true;
    ++next_;
    return true;
  }
  bool RollbackAction(const Config&, const Config&, const PlannedAction& action,
                      AdapterCheckpoint* checkpoint, std::string* error,
                      std::string* path) override {
    if (path) *path = ActionPath(action.id);
    if (!checkpoint || checkpoint->action_id != action.id ||
        (!checkpoint->applied && !transaction_.recovery_pending()))
      return Fail("chrony action has no acknowledged checkpoint", error);
    aborted_ = true;
    if (!restored_) {
      if (!transaction_.Rollback(error)) return false;
      restored_ = true;
    }
    checkpoint->applied = false;
    checkpoint->before_image.clear();
    return true;
  }
  bool OperationalXml(std::string* xml, std::string* error,
                      std::string* path) override {
    return fallback_->OperationalXml(xml, error, path);
  }

 private:
  bool Fail(const char* message, std::string* error) {
    if (error) *error = message;
    return false;
  }
  std::unique_ptr<NativeAdapter> fallback_;
  std::unique_ptr<ChronyService> service_;
  ChronyTransaction transaction_;
  std::string fragment_, keys_;
  ChronyConfigImage image_;
  std::size_t next_ = 0;
  bool captured_ = false, stopped_ = false, installed_ = false;
  bool aborted_ = false, restored_ = false;
};
}  // namespace
std::unique_ptr<NativeAdapter> CreateChronyActionAdapter(
    AdapterDescriptor descriptor, std::unique_ptr<ChronyService> service,
    std::string fragment_path, std::string key_path) {
  return std::make_unique<ActionAdapter>(
      std::move(descriptor), std::move(service), std::move(fragment_path),
      std::move(key_path));
}
}  // namespace dang::ntp
