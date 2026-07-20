#ifndef BENNU_PRIMITIVE_ID_HPP
#define BENNU_PRIMITIVE_ID_HPP

#include <cstdint>

namespace bennu {

enum class PrimitiveId : std::uint8_t {
  inc = 0,
  add = 1,
  equals = 2,
  logical_not = 3,
  iota = 4,
};

} // namespace bennu

#endif
