// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/schema_inventory.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <utility>

namespace {

class TemporarySchemas {
 public:
  TemporarySchemas() {
    path_ = std::filesystem::temp_directory_path() /
        ("dang-frr-schema-test-" + std::to_string(++sequence_));
    std::filesystem::create_directory(path_);
  }
  ~TemporarySchemas() { std::filesystem::remove_all(path_); }
  void Add(std::string_view filename, std::string_view source) const {
    std::ofstream output(path_ / filename, std::ios::binary);
    output << source;
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  inline static unsigned sequence_ = 0;
  std::filesystem::path path_;
};

dang::plugins::frr::YangSchema Module(std::string name, std::string revision,
                                      std::string namespace_uri) {
  dang::plugins::frr::YangSchema schema;
  schema.module_name = std::move(name);
  schema.revision = std::move(revision);
  schema.namespace_uri = std::move(namespace_uri);
  return schema;
}

dang::plugins::frr::YangSchema Submodule(std::string name,
                                         std::string revision,
                                         std::string owner) {
  auto schema = Module(std::move(name), std::move(revision), {});
  schema.belongs_to = std::move(owner);
  schema.is_submodule = true;
  return schema;
}

TEST(FrrSchemaInventoryTest, LoadsMetadataAndResolvesImportClosure) {
  TemporarySchemas files;
  files.Add("common.yang", R"(module common { namespace "urn:common";
    revision 2026-01-01; })");
  files.Add("frr-staticd-routes.yang", R"(
    submodule frr-staticd-routes {
      belongs-to "frr-staticd" { prefix staticd; }
      import "common" { prefix common; }
      revision 2026-02-04;
    })");
  files.Add("frr-staticd.yang", R"(
    module frr-staticd {
      namespace "urn:frr:staticd";
      include "frr-staticd-routes";
      revision 2026-02-03;
    })");
  dang::plugins::frr::SchemaInventoryOptions options;
  options.explicit_directory = files.path();
  std::string error;
  auto inventory = dang::plugins::frr::DiscoverSchemaInventory(options, &error);
  ASSERT_TRUE(inventory) << error;
  ASSERT_EQ(inventory->size(), 3);
  const auto module = std::ranges::find_if(*inventory, [](const auto& schema) {
    return schema.module_name == "frr-staticd";
  });
  const auto submodule =
      std::ranges::find_if(*inventory, [](const auto& schema) {
        return schema.module_name == "frr-staticd-routes";
      });
  ASSERT_NE(module, inventory->end());
  ASSERT_NE(submodule, inventory->end());
  EXPECT_EQ(module->revision, "2026-02-03");
  EXPECT_EQ(module->namespace_uri, "urn:frr:staticd");
  EXPECT_EQ(module->includes,
            std::vector<std::string>({"frr-staticd-routes"}));
  EXPECT_TRUE(submodule->is_submodule);
  EXPECT_EQ(submodule->belongs_to, "frr-staticd");
  EXPECT_EQ(submodule->imports, std::vector<std::string>({"common"}));
  auto closure = dang::plugins::frr::ResolveImportClosure(
      *inventory, {"frr-staticd"}, &error);
  ASSERT_TRUE(closure) << error;
  EXPECT_EQ(closure->size(), 3);
}

TEST(FrrSchemaInventoryTest, DoesNotOverlayAlternativeDirectories) {
  TemporarySchemas first;
  TemporarySchemas second;
  first.Add("frr-one.yang", "module frr-one {}");
  second.Add("frr-two.yang", "module frr-two {}");
  dang::plugins::frr::SchemaInventoryOptions options;
  options.search_directories = {first.path(), second.path()};
  std::string error;
  auto inventory = dang::plugins::frr::DiscoverSchemaInventory(options, &error);
  ASSERT_TRUE(inventory) << error;
  ASSERT_EQ(inventory->size(), 1);
  EXPECT_EQ(inventory->front().module_name, "frr-one");
}

TEST(FrrSchemaInventoryTest, RejectsTruncatedAndMissingDependencySources) {
  TemporarySchemas files;
  files.Add("frr-broken.yang", "module frr-broken { import absent;");
  dang::plugins::frr::SchemaInventoryOptions options;
  options.explicit_directory = files.path();
  std::string error;
  EXPECT_FALSE(dang::plugins::frr::DiscoverSchemaInventory(options, &error));
  EXPECT_NE(error.find("unbalanced"), std::string::npos);

  TemporarySchemas valid;
  valid.Add("frr-root.yang", "module frr-root { import absent; }");
  options.explicit_directory = valid.path();
  auto inventory = dang::plugins::frr::DiscoverSchemaInventory(options, &error);
  ASSERT_TRUE(inventory) << error;
  EXPECT_FALSE(dang::plugins::frr::ResolveImportClosure(
      *inventory, {"frr-root"}, &error));
  EXPECT_NE(error.find("absent"), std::string::npos);

  TemporarySchemas missing_include;
  missing_include.Add(
      "frr-root.yang",
      "module frr-root { namespace \"urn:root\"; include absent-part; }");
  options.explicit_directory = missing_include.path();
  inventory = dang::plugins::frr::DiscoverSchemaInventory(options, &error);
  ASSERT_TRUE(inventory) << error;
  EXPECT_FALSE(dang::plugins::frr::ResolveImportClosure(
      *inventory, {"frr-root"}, &error));
  EXPECT_NE(error.find("absent-part"), std::string::npos);
}

TEST(FrrSchemaInventoryTest, EnforcesSourceSizeLimit) {
  TemporarySchemas files;
  files.Add("frr-large.yang", "module frr-large {}");
  dang::plugins::frr::SchemaInventoryOptions options;
  options.explicit_directory = files.path();
  options.maximum_source_bytes = 4;
  std::string error;
  EXPECT_FALSE(dang::plugins::frr::DiscoverSchemaInventory(options, &error));
  EXPECT_NE(error.find("size limit"), std::string::npos);
}

TEST(FrrSchemaInventoryTest, AppliesRuntimeFeaturesAndRejectsSchemaSkew) {
  std::vector<dang::plugins::frr::YangSchema> schemas{
      Module("frr-zebra", "2019-06-01", "urn:frr:zebra"),
      Submodule("frr-zebra-routes", "2019-06-02", "frr-zebra")};
  std::string error;
  EXPECT_TRUE(dang::plugins::frr::ApplyRuntimeYangLibrary(
      R"(<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library">
           <module-set><name>complete</name><module>
             <name>frr-zebra</name><revision>2019-06-01</revision>
             <namespace>urn:frr:zebra</namespace><feature>ra</feature>
             <submodule><name>frr-zebra-routes</name>
               <revision>2019-06-02</revision></submodule>
           </module></module-set></yang-library>)",
      &schemas, &error)) << error;
  EXPECT_EQ(schemas.front().enabled_features,
            std::vector<std::string>({"ra"}));
  EXPECT_TRUE(schemas[1].enabled_features.empty());

  schemas.front().revision = "different";
  EXPECT_FALSE(dang::plugins::frr::ApplyRuntimeYangLibrary(
      R"(<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library"><module-set><module><name>frr-zebra</name>
           <revision>2019-06-01</revision><namespace>urn:frr:zebra</namespace>
           <submodule><name>frr-zebra-routes</name>
             <revision>2019-06-02</revision></submodule>
         </module></module-set></yang-library>)",
      &schemas, &error));
  EXPECT_NE(error.find("disagree"), std::string::npos);
  EXPECT_EQ(schemas.front().enabled_features,
            std::vector<std::string>({"ra"}));
}

TEST(FrrSchemaInventoryTest, RejectsRuntimeSubmoduleSkew) {
  std::vector<dang::plugins::frr::YangSchema> schemas{
      Module("frr-bgp", "2019-12-03", "http://frrouting.org/yang/bgp"),
      Submodule("frr-bgp-neighbor", "2019-12-03", "frr-bgp")};
  std::string error;
  EXPECT_FALSE(dang::plugins::frr::ApplyRuntimeYangLibrary(
      R"(<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library">
           <module-set><module><name>frr-bgp</name>
             <revision>2019-12-03</revision>
             <namespace>http://frrouting.org/yang/bgp</namespace>
             <submodule><name>frr-bgp-neighbor</name>
               <revision>2026-01-01</revision></submodule>
           </module></module-set></yang-library>)",
      &schemas, &error));
  EXPECT_NE(error.find("submodule frr-bgp-neighbor"), std::string::npos);
}

TEST(FrrSchemaInventoryTest, DistinguishesImplementedFromImportOnlyModules) {
  std::string error;
  auto modules = dang::plugins::frr::RuntimeImplementedModules(
      R"(<yang-library xmlns="urn:ietf:params:xml:ns:yang:ietf-yang-library">
           <module-set><module><name>frr-ripd</name></module>
             <import-only-module><name>ietf-inet-types</name></import-only-module>
           </module-set></yang-library>)",
      &error);
  ASSERT_TRUE(modules) << error;
  EXPECT_EQ(*modules, std::set<std::string>({"frr-ripd"}));
}

}  // namespace
