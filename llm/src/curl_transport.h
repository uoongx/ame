#pragma once

#include <chrono>
#include <string>

namespace ame::detail {
// Internal transport seam; never invokes a shell or puts credentials in argv.
int curl_transport(const std::string& base_url, const std::string& api_key,
                   const std::string& body, std::string* out);
// The budget covers setup, output capture and child exit, capped at 120 seconds.
int curl_transport(const std::string& base_url, const std::string& api_key,
                   const std::string& body, std::string* out,
                   std::chrono::milliseconds timeout);
}  // namespace ame::detail
