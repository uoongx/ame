#include "retry_backoff.h"

namespace ame::detail {

std::chrono::milliseconds retry_backoff(int attempt) {
  // Each worker owns its generator; randomness is for scheduling, not security.
  thread_local std::mt19937 generator{retry_seed([] { return std::random_device{}(); })};
  return retry_backoff(attempt, generator);
}

}  // namespace ame::detail
