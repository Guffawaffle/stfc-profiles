// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#include "stfc_profiles/identity.h"

#include <stdexcept>
#include <string_view>

void Check(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

int main()
{
  using namespace stfc::profiles;
  Check(ValidId("josep") && ValidId("dev_2") && ValidId("a-1"), "valid IDs");
  Check(ValidId("0123456789abcdef0123456789abcdef"), "Bridge GUID ID");
  Check(!ValidId("") && !ValidId("JOS") && !ValidId("con") && !ValidId("lpt9"), "invalid IDs");
  Check(!ValidId("with space") && !ValidId("bad/path") && !ValidId("a\n"), "unsafe IDs");
  Check(!ValidId("123456789012345678901234567890123"), "long ID");

}
