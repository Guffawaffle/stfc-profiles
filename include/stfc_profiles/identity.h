// Derived from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#pragma once

#include <string_view>

namespace stfc::profiles {

inline bool ValidId(std::string_view id)
{
  if (id.empty() || id.size() > 32)
    return false;
  for (const char ch : id)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
      return false;
  if (id == "con" || id == "prn" || id == "aux" || id == "nul")
    return false;
  return !(id.size() == 4 && id[3] >= '1' && id[3] <= '9'
           && (id.substr(0, 3) == "com" || id.substr(0, 3) == "lpt"));
}

} // namespace stfc::profiles
