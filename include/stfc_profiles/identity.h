// Derived from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#pragma once

#include <string_view>

namespace stfc::profiles {

inline bool ValidId(std::string_view id)
{
  return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == id.npos;
}

} // namespace stfc::profiles
