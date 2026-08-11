#pragma once

#include "catalog.hpp"

#include <vector>

namespace fusioncutter::catalog {

[[nodiscard]] std::vector<CatalogEntry> generated_catalog_entries();

} // namespace fusioncutter::catalog
