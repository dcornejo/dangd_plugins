// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "plugins/frr/src/schema_inventory.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

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

TEST(FrrSchemaInventoryTest, LoadsMetadataAndResolvesImportClosure) {
  TemporarySchemas files;
  files.Add("common.yang", R"(module common { revision 2026-01-01; })");
  files.Add("frr-staticd.yang", R"(
    module frr-staticd {
      // import ignored-comment;
      description "import ignored-string;";
      import common { prefix common; }
      revision 2026-02-03;
    })");
  dang::plugins::frr::SchemaInventoryOptions options;
  options.explicit_directory = files.path();
  std::string error;
  auto inventory = dang::plugins::frr::DiscoverSchemaInventory(options, &error);
  ASSERT_TRUE(inventory) << error;
  ASSERT_EQ(inventory->size(), 2);
  EXPECT_EQ((*inventory)[1].module_name, "frr-staticd");
  EXPECT_EQ((*inventory)[1].revision, "2026-02-03");
  EXPECT_EQ((*inventory)[1].imports,
            std::vector<std::string>({"common"}));
  auto closure = dang::plugins::frr::ResolveImportClosure(
      *inventory, {"frr-staticd"}, &error);
  ASSERT_TRUE(closure) << error;
  EXPECT_EQ(closure->size(), 2);
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

}  // namespace
