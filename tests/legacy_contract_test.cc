// Extracted from Guffawaffle/stfc-mod at 323fb857f51f4cb08231d4b150ea8b5bb59340d1.
// See docs/PROVENANCE.json and LICENSE (GPL-3.0).
#include "stfc_profiles/legacy_contract.h"

#include <stdexcept>
#include <string_view>

void Check(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

int main()
{
  using namespace stfc::profiles::legacy;
  Check(ValidId("josep") && ValidId("dev_2") && ValidId("a-1"), "valid IDs");
  Check(ValidId("0123456789abcdef0123456789abcdef"), "Bridge GUID ID");
  Check(!ValidId("") && !ValidId("JOS") && !ValidId("con") && !ValidId("lpt9"), "invalid IDs");
  Check(!ValidId("with space") && !ValidId("bad/path") && !ValidId("a\n"), "unsafe IDs");
  Check(!ValidId("123456789012345678901234567890123"), "long ID");

  Check(ParseMarker("v1:josep") == "josep", "marker without newline");
  Check(ParseMarker("v1:josep\n") == "josep", "LF marker");
  Check(ParseMarker("v1:josep\r\n") == "josep", "CRLF marker");
  for (const std::string_view invalid : {"josep", "v2:josep", "v1:", "v1:JOS", "v1:con", "v1:josep\n\n",
                                         "v1:josep ", "v1:josep\r", "\xef\xbb\xbfv1:josep"})
    Check(!ParseMarker(invalid), "invalid marker accepted");

  constexpr std::string_view path{R"(c:\games\josep\game)"};
  const auto                  bound = Receipt("josep", path);
  Check(bound == "v2:profile-prefs-v1\njosep\nc:\\games\\josep\\game\n", "receipt bytes");
  Check(ReceiptClaimsId(bound, "josep"), "receipt identity");
  Check(!ReceiptClaimsId(bound, "jos") && !ReceiptClaimsId(bound, "other"), "receipt identity boundary");
  Check(!ReceiptClaimsId("v1:josep\n", "josep"), "old receipt identity");
  Check(PathHash("") == 0xcbf29ce484222325ull, "empty path hash");
  Check(PathHash("a") == 0xaf63dc4c8601ec8cull, "path hash vector");

  using enum SelectionState;
  Check(Decide({}, {}, {}, path).state == Default, "unmarked default");
  Check(Decide({}, bound, {}, path).state == MissingMarker, "lost marker");
  Check(Decide({}, {}, bound, path).state == MissingMarker, "lost marker during enrollment");
  Check(Decide("v1:JOSEP", {}, {}, path).state == InvalidMarker, "bad marker");
  Check(Decide("v1:josep", {}, {}, path).state == Enroll, "first enrollment");
  Check(Decide("v1:josep", {}, bound, path).state == Resume, "interrupted enrollment");
  Check(Decide("v1:josep", bound, {}, path).state == Bound, "bound install");
  Check(Decide("v1:josep", bound, bound, path).state == Bound, "completed enrollment with pending receipt");
  Check(Decide("v1:josep", bound, "wrong", path).state == ReceiptConflict, "pending conflict");
  Check(Decide("v1:josep", bound, {}, "different path").state == ReceiptConflict, "moved install");
  Check(Decide("v1:other", bound, {}, path).state == ReceiptConflict, "changed identity");
  Check(Decide("v1:josep", "v1:josep\nc:\\games\\josep\\game\n", {}, path).state == ReceiptConflict,
        "registry-prefix receipt accepted");
}
