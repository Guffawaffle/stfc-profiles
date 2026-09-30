#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace stfc::profiles {

class CatalogError : public std::runtime_error {
public:
  CatalogError(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code_(std::move(code)) {}
  const std::string& Code() const noexcept { return code_; }
private:
  std::string code_;
};

std::filesystem::path DefaultCatalogRoot();

// Version 1 UTF-8 JSON request/response. The catalog owns all metadata and
// lifecycle mutations. Account secrets are never part of this interface.
// Request fields: apiVersion, operation, root?, id?, name?, gameDirectory?,
// expectedRevision?, archived?. Runtime launch returns ready only after the
// requested process identity has reported installed isolation.
std::string ExecuteCatalogRequest(std::string_view request_utf8);

} // namespace stfc::profiles
