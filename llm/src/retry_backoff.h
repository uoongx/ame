#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <random>

namespace ame::detail {

inline constexpr int kMaxTransientFailures = 5;
inline constexpr uint32_t kBaseBackoffMs = 1000U;

template <typename Entropy>
std::mt19937::result_type retry_seed(Entropy entropy) {
  try {
    return entropy();
  } catch (const std::exception&) {
    // An unavailable entropy source must not terminate a retrying worker.
    static std::atomic<uint32_t> sequence{0U};
    const auto ticks = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return static_cast<uint32_t>(ticks ^ (ticks >> 32U)) ^ sequence.fetch_add(1U);
  }
}

// Full jitter spreads concurrent retries across each bounded exponential window.
template <typename Generator>
std::chrono::milliseconds retry_backoff(int attempt, Generator& generator) {
  if (attempt <= 0) return std::chrono::milliseconds::zero();
  const auto exponent = static_cast<unsigned>(std::min(attempt, kMaxTransientFailures - 1) - 1);
  const uint32_t ceiling_ms = kBaseBackoffMs * (1U << exponent);
  std::uniform_int_distribution<uint32_t> jitter(0U, ceiling_ms);
  return std::chrono::milliseconds(jitter(generator));
}

std::chrono::milliseconds retry_backoff(int attempt);

}  // namespace ame::detail
