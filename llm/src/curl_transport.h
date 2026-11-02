#pragma once

#include <string>

namespace ame::detail {
// Internal transport seam; never invokes a shell or puts credentials in argv.
int curl_transport(const std::string& base_url, const std::string& api_key,
                   const std::string& body, std::string* out);
}  // namespace ame::detail
