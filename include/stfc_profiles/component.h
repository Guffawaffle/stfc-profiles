#pragma once

#include <string_view>

namespace stfc::profiles {

// Source-library interface version, not shared-install or game readiness.
std::string_view ComponentVersion() noexcept;
std::string_view ExtractionSourceRevision() noexcept;

} // namespace stfc::profiles
