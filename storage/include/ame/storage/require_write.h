#pragma once

#include "ame/core/types.h"

#include <stdexcept>
#include <string>

namespace ame {

inline void require_write(Err result) {
  if (result != Err::Ok) throw std::runtime_error(std::string("Graph write failed: ") + err_name(result));
}

}  // namespace ame
