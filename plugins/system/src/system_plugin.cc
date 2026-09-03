// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "plugins/system/src/auth_server.h"
#include "plugins/system/src/platform.h"
#include "plugins/system/src/system_config.h"
#include "system_model_sources.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace {

using dang::system::Config;

struct Context {
  std::mutex mutex;
  Config applied;
  dang::system::AuthServer authentication;
  std::string error;
  std::string path;
  std::string authentication_startup_error;
  std::string operational;

  Context() {
    const char* socket = std::getenv("DANG_SYSTEM_AUTH_SOCKET");
    if (socket && *socket) {
      (void)authentication.Start(
          socket,
          [this](std::string_view username, std::string_view password) {
            std::lock_guard lock(mutex);
            if (!applied.local_password_authentication) return false;
            for (const auto& user : applied.users)
              if (user.name == username && user.password_hash)
                return dang::system::VerifyPassword(password,
                                                    *user.password_hash);
            return false;
          },
          &authentication_startup_error);
    }
  }
};

struct Prepared {
  dang::system::PreparedPlatform platform;
};

Context context;

int Fail(Context* owner, DangPluginErrorV1* error, std::string message,
         std::string path) {
  owner->error = std::move(message);
  owner->path = std::move(path);
  if (error) {
    error->message = owner->error.c_str();
    error->instance_path = owner->path.c_str();
  }
  return 0;
}

std::size_t SourceCount(void*) { return 2; }

int SourceAt(void*, std::size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  static constexpr const char* system_features[] = {
      "authentication", "local-users", "ntp", "timezone-name"};
  static constexpr const char* crypt_features[] = {
      "crypt-hash-sha-256", "crypt-hash-sha-512"};
  if (!source || index > 1) return -1;
  if (index == 0) {
    *source = {.module_name = "ietf-system",
               .revision = "2014-08-06",
               .source = kIetfSystemYang.data(),
               .source_size = kIetfSystemYang.size(),
               .source_uri = "urn:ietf:rfc:7317",
               .role = DANG_YANG_IMPLEMENTED_V1,
               .enabled_features = system_features,
               .enabled_feature_count = std::size(system_features)};
  } else {
    *source = {.module_name = "iana-crypt-hash",
               .revision = "2014-08-06",
               .source = kIanaCryptHashYang.data(),
               .source_size = kIanaCryptHashYang.size(),
               .source_uri = "urn:ietf:rfc:7317",
               .role = DANG_YANG_IMPORT_ONLY_V1,
               .enabled_features = crypt_features,
               .enabled_feature_count = std::size(crypt_features)};
  }
  return 1;
}

int Prepare(void* raw, const DangTransactionV1* transaction, void** output,
            DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!transaction || !output)
    return Fail(owner, error, "missing transaction", "/ietf-system:system");
  if (!owner->authentication_startup_error.empty())
    return Fail(owner, error, owner->authentication_startup_error,
                "/ietf-system:system/authentication");
  auto prepared = std::make_unique<Prepared>();
  Config before;
  Config proposed;
  if (!dang::system::ParseConfig(transaction->before_xml, &before,
                                 &owner->error, &owner->path) ||
      !dang::system::ParseConfig(transaction->proposed_xml, &proposed,
                                 &owner->error, &owner->path))
    return Fail(owner, error, owner->error, owner->path);
  if (!dang::system::PreparePlatform(before, proposed, &prepared->platform,
                                     &owner->error, &owner->path))
    return Fail(owner, error, owner->error, owner->path);
  *output = prepared.release();
  return 1;
}

int Validate(void* raw, void* prepared, DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!prepared)
    return Fail(owner, error, "missing prepared transaction",
                "/ietf-system:system");
  if (!dang::system::ValidatePlatform(
          static_cast<Prepared*>(prepared)->platform, &owner->error,
          &owner->path))
    return Fail(owner, error, owner->error, owner->path);
  return 1;
}

int Apply(void* raw, void* prepared, DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  auto* plan = static_cast<Prepared*>(prepared);
  if (!plan || !dang::system::ApplyPlatform(&plan->platform, &owner->error,
                                            &owner->path))
    return Fail(owner, error, owner->error.empty() ? "apply failed" : owner->error,
                owner->path.empty() ? "/ietf-system:system" : owner->path);
  {
    std::lock_guard lock(owner->mutex);
    owner->applied = plan->platform.proposed;
  }
  return 1;
}

int Rollback(void* raw, void* prepared, DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  auto* plan = static_cast<Prepared*>(prepared);
  if (!plan || !dang::system::RollbackPlatform(&plan->platform, &owner->error,
                                               &owner->path))
    return Fail(owner, error,
                owner->error.empty() ? "rollback failed" : owner->error,
                owner->path.empty() ? "/ietf-system:system" : owner->path);
  {
    std::lock_guard lock(owner->mutex);
    owner->applied = plan->platform.before;
  }
  return 1;
}

void Release(void*, void* prepared) { delete static_cast<Prepared*>(prepared); }

int Invoke(void* raw, const DangOperationV1* operation,
           DangOperationResultV1* result, DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  if (!operation || operation->module_name != std::string_view("ietf-system"))
    return Fail(owner, error, "unsupported operation", "/ietf-system:system");
  std::string operation_error;
  bool okay = false;
  if (operation->operation_name == std::string_view("set-current-datetime")) {
    {
      std::lock_guard lock(owner->mutex);
      if (owner->applied.ntp_present && owner->applied.ntp_enabled)
        return Fail(owner, error, "NTP is active (ntp-active)",
                    "/ietf-system:set-current-datetime/current-datetime");
    }
    okay = dang::system::SetCurrentDatetime(
        operation->input_xml ? operation->input_xml : "", &operation_error);
  } else if (operation->operation_name == std::string_view("system-restart")) {
    okay = dang::system::RequestPowerOperation(true, &operation_error);
  } else if (operation->operation_name == std::string_view("system-shutdown")) {
    okay = dang::system::RequestPowerOperation(false, &operation_error);
  } else {
    operation_error = "unsupported ietf-system operation";
  }
  if (!okay)
    return Fail(owner, error, operation_error, operation->instance_path
                                                    ? operation->instance_path
                                                    : "/ietf-system:system");
  if (result) result->output_xml = "";
  return 1;
}

int Operational(void* raw, DangOperationalDataV1* result,
                DangPluginErrorV1* error) {
  auto* owner = static_cast<Context*>(raw);
  owner->operational = dang::system::OperationalStateXml();
  if (owner->operational.empty())
    return Fail(owner, error, "cannot read native platform state",
                "/ietf-system:system-state");
  result->data_xml = owner->operational.c_str();
  return 1;
}

const DangPluginV3 plugin = {
    .v2 = {.v1 = {.abi_version = DANG_PLUGIN_ABI_V3,
                  .plugin_name = "ietf-system",
                  .context = &context,
                  .yang_source_count = SourceCount,
                  .yang_source_at = SourceAt,
                  .dependency_count = nullptr,
                  .dependency_at = nullptr,
                  .prepare = Prepare,
                  .validate = Validate,
                  .apply = Apply,
                  .rollback = Rollback,
                  .release = Release,
                  .destroy = nullptr},
           .invoke = Invoke},
    .get_operational_data = Operational};

}  // namespace

extern "C" const DangPluginV3* dang_plugin_init_v3() { return &plugin; }
