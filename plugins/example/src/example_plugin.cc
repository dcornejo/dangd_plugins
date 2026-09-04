// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * Minimal transaction plugin used as both an executable example and an ABI
 * smoke test.  It intentionally has no external side effects: apply and
 * rollback print the proposed and retained configurations so developers can
 * see the lifetime and ordering guarantees made by dangd.
 */

#include "dangd/plugin_api.h"

#include "example_model_source.h"

#include <cstring>
#include <new>
#include <string>

namespace {

struct PreparedConfiguration {
  std::string before;
  std::string proposed;
};

std::string applied_configuration;

size_t YangSourceCount(void*) { return 1; }

int YangSourceAt(void*, size_t index, DangYangSourceV1* source,
                 DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dang-plugins-example",
             "2026-08-25",
             kExampleYangSource,
             std::strlen(kExampleYangSource),
             "plugin:dang-plugins-example",
             DANG_YANG_IMPLEMENTED_V1,
             nullptr,
             0};
  return 1;
}

size_t DependencyCount(void*) { return 0; }

const char* DependencyAt(void*, size_t) { return nullptr; }

int Prepare(void*, const DangTransactionV1* transaction, void** prepared,
            DangPluginErrorV1* error) {
  if (!transaction || !transaction->before_xml || !transaction->proposed_xml ||
      !prepared) {
    if (error) error->message = "the transaction snapshot is incomplete";
    return 0;
  }
  auto* retained = new (std::nothrow) PreparedConfiguration{
      transaction->before_xml, transaction->proposed_xml};
  if (!retained) {
    if (error) error->message = "cannot retain the transaction plan";
    return 0;
  }
  *prepared = retained;
  return 1;
}

int Validate(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<PreparedConfiguration*>(opaque);
  if (!prepared) {
    if (error) error->message = "the prepared transaction is missing";
    return 0;
  }
  if (prepared->proposed.find("<mode>reject</mode>") == std::string::npos)
    return 1;
  if (error) {
    error->message = "mode 'reject' is not supported by this provider";
    error->instance_path =
        "/{urn:dang:plugins:example}example-settings/"
        "{urn:dang:plugins:example}mode";
  }
  return 0;
}

int Apply(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<PreparedConfiguration*>(opaque);
  if (!prepared) {
    if (error) error->message = "the prepared transaction is missing";
    return 0;
  }
  applied_configuration = prepared->proposed;
  return 1;
}

int Rollback(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<PreparedConfiguration*>(opaque);
  if (!prepared) {
    if (error) error->message = "the prepared transaction is missing";
    return 0;
  }
  applied_configuration = prepared->before;
  return 1;
}

void Release(void*, void* opaque) {
  delete static_cast<PreparedConfiguration*>(opaque);
}

const DangPluginV1 kPlugin{
    DANG_PLUGIN_ABI_V1,
    "dang-plugins-example",
    nullptr,
    YangSourceCount,
    YangSourceAt,
    DependencyCount,
    DependencyAt,
    Prepare,
    Validate,
    Apply,
    Rollback,
    Release,
    nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }
