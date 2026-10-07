#pragma once
#include <string_view>
namespace stfc::profiles {
// macOS effective preferences include managed policy. A storage override means
// the browser cannot honor this product's private user-data directory.
bool MacBrowserAllowsPrivateStore(std::string_view preference_domain);
}
