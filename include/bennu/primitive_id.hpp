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
  logical_and = 5,
  logical_or = 6,
  not_equals = 7,
  odd = 8,
  even = 9,
  is_positive = 10,
  is_negative = 11,
  less_than = 12,
  greater_than = 13,
};

} // namespace bennu

#endif
