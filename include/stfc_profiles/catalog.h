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

// UTF-8 JSON apiVersion 1 (isolated profiles) or 2 (typed profiles). The catalog owns all metadata and
// lifecycle mutations. Account secrets are never part of this interface.
// Request fields: apiVersion, operation, root?, id?, name?, gameDirectory?,
// expectedRevision?, archived?. Runtime launch returns ready only after the
// requested process identity has reported installed isolation. Version 2 adds
// ensure-default, resolve-default and launch-ordinary for the metadata-only
// windows-user descriptor; ordinary startup never claims isolation readiness.
// Version 2 also owns installations/register-installation/installation-paths,
// explicit installationId and revision-bound preferredInstallationId imports.
// macOS launch additionally requires runtimeLibrary: an absolute profile-aware
// dylib path. gameDirectory is the selected app's Contents/MacOS directory.
std::string ExecuteCatalogRequest(std::string_view request_utf8);

} // namespace stfc::profiles
