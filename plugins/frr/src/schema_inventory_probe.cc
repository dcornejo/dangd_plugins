// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "schema_inventory.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: frr-schema-inventory YANG_DIRECTORY ROOT_MODULE...\n";
    return 2;
  }
  dang::plugins::frr::SchemaInventoryOptions options;
  options.explicit_directory = argv[1];
  std::string error;
  auto inventory = dang::plugins::frr::DiscoverSchemaInventory(options, &error);
  if (!inventory) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<std::string> roots;
  for (int index = 2; index < argc; ++index) roots.emplace_back(argv[index]);
  auto closure =
      dang::plugins::frr::ResolveImportClosure(*inventory, roots, &error);
  if (!closure) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "inventory=" << inventory->size()
            << " closure=" << closure->size() << '\n';
  for (const std::size_t index : *closure)
    std::cout << (*inventory)[index].module_name << '@'
              << ((*inventory)[index].revision.empty()
                      ? "unrevisioned"
                      : (*inventory)[index].revision)
              << '\n';
  return 0;
}
