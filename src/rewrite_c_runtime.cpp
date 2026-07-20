#include "rewrite_c_runtime.hpp"

namespace bennu {

void append_rewrite_c_runtime(std::string &source) {
  source += R"bennu_c(/* Generated deterministically by Bennu. Standard C11. */
#include <inttypes.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum BennuType {
  BENNU_BOOL = 0,
  BENNU_INT = 1,
  BENNU_DOUBLE = 2
} BennuType;

typedef enum BennuContainer {
  BENNU_SCALAR = 0,
  BENNU_VECTOR = 1
} BennuContainer;

typedef enum BennuImplementation {
  BENNU_IMPL_INC_INT = 1,
  BENNU_IMPL_INC_DOUBLE = 2,
  BENNU_IMPL_ADD_INT = 3,
  BENNU_IMPL_ADD_DOUBLE = 4,
  BENNU_IMPL_EQUALS_BOOL = 5,
  BENNU_IMPL_EQUALS_INT = 6,
  BENNU_IMPL_EQUALS_DOUBLE = 7,
  BENNU_IMPL_NOT_BOOL = 8,
  BENNU_IMPL_IOTA_INT = 9
} BennuImplementation;

typedef enum BennuFailure {
  BENNU_FAILURE_NONE = 0,
  BENNU_FAILURE_SIZE = 1,
  BENNU_FAILURE_PROFILE = 2,
  BENNU_FAILURE_ALLOCATION = 3,
  BENNU_FAILURE_DOMAIN = 4,
  BENNU_FAILURE_INTERNAL = 5
} BennuFailure;

typedef struct BennuScalar {
  BennuType type;
  uint8_t boolean;
  int64_t integer;
  double double_precision;
} BennuScalar;

typedef struct BennuValue {
  BennuContainer container;
  BennuType type;
  size_t count;
  uint8_t boolean;
  int64_t integer;
  double double_precision;
  void *data;
} BennuValue;

typedef struct BennuResources {
  int has_vector_limit;
  int has_live_limit;
  int has_work_limit;
  size_t vector_limit;
  size_t live_limit;
  size_t work_limit;
  size_t live_bytes;
  size_t work_units;
  size_t reservation_ordinal;
  int has_failure_ordinal;
  size_t failure_ordinal;
  BennuFailure failure;
} BennuResources;

static size_t bennu_width(BennuType type) {
  return type == BENNU_BOOL ? (size_t)1 : (size_t)8;
}

static uint64_t bennu_double_bits(double value) {
  uint64_t bits = UINT64_C(0);
  (void)memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static double bennu_double_from_bits(uint64_t bits) {
  double value = 0.0;
  (void)memcpy(&value, &bits, sizeof(value));
  return value;
}

static double bennu_normalize_double(double value) {
  const uint64_t bits = bennu_double_bits(value);
  if ((bits & UINT64_C(0x7ff0000000000000)) ==
          UINT64_C(0x7ff0000000000000) &&
      (bits & UINT64_C(0x000fffffffffffff)) != UINT64_C(0)) {
    return bennu_double_from_bits(UINT64_C(0x7ff8000000000000));
  }
  return value;
}

static void bennu_set_failure(BennuResources *resources,
                              BennuFailure failure) {
  if (resources->failure == BENNU_FAILURE_NONE) {
    resources->failure = failure;
  }
}

static int bennu_charge_work(BennuResources *resources, size_t work) {
  if (work > SIZE_MAX - resources->work_units) {
    bennu_set_failure(resources, BENNU_FAILURE_SIZE);
    return 0;
  }
  if (resources->has_work_limit != 0 &&
      resources->work_units + work > resources->work_limit) {
    bennu_set_failure(resources, BENNU_FAILURE_PROFILE);
    return 0;
  }
  resources->work_units += work;
  return 1;
}

static int bennu_allocate(BennuResources *resources, BennuValue *value,
                          BennuType type, size_t count, size_t work) {
  const size_t width = bennu_width(type);
  size_t bytes = 0U;
  size_t live_after = 0U;
  size_t work_after = 0U;
  void *data = NULL;
  if (count > SIZE_MAX / width || work > SIZE_MAX - resources->work_units) {
    bennu_set_failure(resources, BENNU_FAILURE_SIZE);
    return 0;
  }
  bytes = count * width;
  if (bytes > SIZE_MAX - resources->live_bytes) {
    bennu_set_failure(resources, BENNU_FAILURE_SIZE);
    return 0;
  }
  live_after = resources->live_bytes + bytes;
  work_after = resources->work_units + work;
  if (resources->has_vector_limit != 0 && bytes > resources->vector_limit) {
    bennu_set_failure(resources, BENNU_FAILURE_PROFILE);
    return 0;
  }
  if (resources->has_live_limit != 0 && live_after > resources->live_limit) {
    bennu_set_failure(resources, BENNU_FAILURE_PROFILE);
    return 0;
  }
  if (resources->has_work_limit != 0 && work_after > resources->work_limit) {
    bennu_set_failure(resources, BENNU_FAILURE_PROFILE);
    return 0;
  }
  if (bytes != 0U) {
    const size_t ordinal = resources->reservation_ordinal;
    resources->reservation_ordinal += 1U;
    if (resources->has_failure_ordinal != 0 &&
        ordinal == resources->failure_ordinal) {
      bennu_set_failure(resources, BENNU_FAILURE_ALLOCATION);
      return 0;
    }
    data = malloc(bytes);
    if (data == NULL) {
      bennu_set_failure(resources, BENNU_FAILURE_ALLOCATION);
      return 0;
    }
    (void)memset(data, 0, bytes);
  }
  resources->live_bytes = live_after;
  resources->work_units = work_after;
  value->container = BENNU_VECTOR;
  value->type = type;
  value->count = count;
  value->data = data;
  return 1;
}

static void bennu_release(BennuResources *resources, BennuValue *value) {
  if (value->container == BENNU_VECTOR) {
    const size_t bytes = value->count * bennu_width(value->type);
    if (bytes <= resources->live_bytes) {
      resources->live_bytes -= bytes;
    }
    free(value->data);
  }
  (void)memset(value, 0, sizeof(*value));
}

static BennuValue bennu_scalar_bool(uint8_t value) {
  BennuValue result = {0};
  result.container = BENNU_SCALAR;
  result.type = BENNU_BOOL;
  result.count = 1U;
  result.boolean = value;
  return result;
}

static BennuValue bennu_scalar_int(int64_t value) {
  BennuValue result = {0};
  result.container = BENNU_SCALAR;
  result.type = BENNU_INT;
  result.count = 1U;
  result.integer = value;
  return result;
}

static BennuValue bennu_scalar_double_bits(uint64_t bits) {
  BennuValue result = {0};
  result.container = BENNU_SCALAR;
  result.type = BENNU_DOUBLE;
  result.count = 1U;
  result.double_precision =
      bennu_normalize_double(bennu_double_from_bits(bits));
  return result;
}

static int bennu_literal_bool(BennuResources *resources, BennuValue *result,
                              const uint8_t *values, size_t count) {
  if (!bennu_allocate(resources, result, BENNU_BOOL, count, 0U)) {
    return 0;
  }
  if (count != 0U) {
    (void)memcpy(result->data, values, count);
  }
  return 1;
}

static int bennu_literal_int(BennuResources *resources, BennuValue *result,
                             const int64_t *values, size_t count) {
  if (!bennu_allocate(resources, result, BENNU_INT, count, 0U)) {
    return 0;
  }
  if (count != 0U) {
    (void)memcpy(result->data, values, count * sizeof(int64_t));
  }
  return 1;
}

static int bennu_literal_double(BennuResources *resources,
                                BennuValue *result, const uint64_t *values,
                                size_t count) {
  size_t index = 0U;
  double *output = NULL;
  if (!bennu_allocate(resources, result, BENNU_DOUBLE, count, 0U)) {
    return 0;
  }
  output = (double *)result->data;
  for (index = 0U; index < count; ++index) {
    output[index] = bennu_normalize_double(bennu_double_from_bits(values[index]));
  }
  return 1;
}

static int bennu_literal(BennuResources *resources, BennuValue *result,
                         BennuType type, const void *values, size_t count) {
  if (type == BENNU_BOOL) {
    return bennu_literal_bool(resources, result, (const uint8_t *)values,
                              count);
  }
  if (type == BENNU_INT) {
    return bennu_literal_int(resources, result, (const int64_t *)values,
                             count);
  }
  return bennu_literal_double(resources, result, (const uint64_t *)values,
                              count);
}

static BennuScalar bennu_project(const BennuValue *value, size_t index) {
  BennuScalar result = {value->type, 0U, INT64_C(0), 0.0};
  if (value->container == BENNU_SCALAR) {
    result.boolean = value->boolean;
    result.integer = value->integer;
    result.double_precision = value->double_precision;
  } else if (value->type == BENNU_BOOL) {
    result.boolean = ((const uint8_t *)value->data)[index];
  } else if (value->type == BENNU_INT) {
    result.integer = ((const int64_t *)value->data)[index];
  } else {
    result.double_precision = ((const double *)value->data)[index];
  }
  return result;
}

static double bennu_int_to_double(int64_t value) {
  volatile int64_t source = value;
  volatile double converted = (double)source;
  return converted;
}

static BennuScalar bennu_convert(BennuScalar value, BennuType type) {
  if (value.type == BENNU_INT && type == BENNU_DOUBLE) {
    value.type = BENNU_DOUBLE;
    value.double_precision = bennu_int_to_double(value.integer);
    value.integer = INT64_C(0);
  }
  return value;
}

static int bennu_add_int(int64_t left, int64_t right, int64_t *result) {
  if ((right > INT64_C(0) && left > INT64_MAX - right) ||
      (right < INT64_C(0) && left < INT64_MIN - right)) {
    return 0;
  }
  *result = left + right;
  return 1;
}

static double bennu_add_double(double left, double right) {
  volatile double volatile_left = left;
  volatile double volatile_right = right;
  volatile double result = volatile_left + volatile_right;
  return bennu_normalize_double(result);
}
)bennu_c";
  source += R"bennu_c(
static BennuType bennu_result_type(BennuImplementation implementation) {
  if (implementation == BENNU_IMPL_EQUALS_BOOL ||
      implementation == BENNU_IMPL_EQUALS_INT ||
      implementation == BENNU_IMPL_EQUALS_DOUBLE ||
      implementation == BENNU_IMPL_NOT_BOOL) {
    return BENNU_BOOL;
  }
  if (implementation == BENNU_IMPL_INC_DOUBLE ||
      implementation == BENNU_IMPL_ADD_DOUBLE) {
    return BENNU_DOUBLE;
  }
  return BENNU_INT;
}

static int bennu_store(BennuValue *value, size_t index, BennuScalar scalar) {
  if (value->type == BENNU_BOOL) {
    ((uint8_t *)value->data)[index] = scalar.boolean;
  } else if (value->type == BENNU_INT) {
    ((int64_t *)value->data)[index] = scalar.integer;
  } else {
    ((double *)value->data)[index] = scalar.double_precision;
  }
  return 1;
}

static int bennu_kernel(BennuResources *resources,
                        BennuImplementation implementation,
                        BennuScalar left, BennuScalar right,
                        BennuScalar *result) {
  result->type = bennu_result_type(implementation);
  result->boolean = 0U;
  result->integer = INT64_C(0);
  result->double_precision = 0.0;
  if (implementation == BENNU_IMPL_INC_INT) {
    if (!bennu_add_int(left.integer, INT64_C(1), &result->integer)) {
      bennu_set_failure(resources, BENNU_FAILURE_DOMAIN);
      return 0;
    }
  } else if (implementation == BENNU_IMPL_INC_DOUBLE) {
    result->double_precision = bennu_add_double(left.double_precision, 1.0);
  } else if (implementation == BENNU_IMPL_ADD_INT) {
    if (!bennu_add_int(left.integer, right.integer, &result->integer)) {
      bennu_set_failure(resources, BENNU_FAILURE_DOMAIN);
      return 0;
    }
  } else if (implementation == BENNU_IMPL_ADD_DOUBLE) {
    result->double_precision =
        bennu_add_double(left.double_precision, right.double_precision);
  } else if (implementation == BENNU_IMPL_EQUALS_BOOL) {
    result->boolean = (uint8_t)(left.boolean == right.boolean);
  } else if (implementation == BENNU_IMPL_EQUALS_INT) {
    result->boolean = (uint8_t)(left.integer == right.integer);
  } else if (implementation == BENNU_IMPL_EQUALS_DOUBLE) {
    result->boolean = (uint8_t)(left.double_precision == right.double_precision);
  } else if (implementation == BENNU_IMPL_NOT_BOOL) {
    result->boolean = (uint8_t)(left.boolean == 0U);
  } else {
    bennu_set_failure(resources, BENNU_FAILURE_INTERNAL);
    return 0;
  }
  return 1;
}

static int bennu_apply(BennuResources *resources,
                       BennuImplementation implementation,
                       BennuValue *result, const BennuValue *left,
                       const BennuValue *right, size_t argument_count) {
  size_t count = 1U;
  size_t index = 0U;
  int vector_result = 0;
  BennuType parameter_type = BENNU_INT;
  BennuScalar empty = {BENNU_INT, 0U, INT64_C(0), 0.0};
  if (implementation == BENNU_IMPL_IOTA_INT) {
    int64_t bound = left->integer;
    if (bound > INT64_C(0)) {
      const uint64_t unsigned_bound = (uint64_t)bound;
      if (unsigned_bound > (uint64_t)SIZE_MAX) {
        bennu_set_failure(resources, BENNU_FAILURE_SIZE);
        return 0;
      }
      count = (size_t)unsigned_bound;
    } else {
      count = 0U;
    }
    if (!bennu_allocate(resources, result, BENNU_INT, count, count)) {
      return 0;
    }
    for (index = 0U; index < count; ++index) {
      ((int64_t *)result->data)[index] = (int64_t)index + INT64_C(1);
    }
    return 1;
  }
  if (left->container == BENNU_VECTOR) {
    vector_result = 1;
    count = left->count;
  }
  if (argument_count == 2U && right->container == BENNU_VECTOR) {
    vector_result = 1;
    count = right->count;
  }
  if (implementation == BENNU_IMPL_INC_DOUBLE ||
      implementation == BENNU_IMPL_ADD_DOUBLE ||
      implementation == BENNU_IMPL_EQUALS_DOUBLE) {
    parameter_type = BENNU_DOUBLE;
  } else if (implementation == BENNU_IMPL_EQUALS_BOOL ||
             implementation == BENNU_IMPL_NOT_BOOL) {
    parameter_type = BENNU_BOOL;
  }
  if (vector_result != 0) {
    if (!bennu_allocate(resources, result, bennu_result_type(implementation),
                        count, count)) {
      return 0;
    }
  } else if (!bennu_charge_work(resources, 1U)) {
    return 0;
  }
  for (index = 0U; index < count; ++index) {
    BennuScalar left_scalar =
        bennu_convert(bennu_project(left, index), parameter_type);
    BennuScalar right_scalar = empty;
    BennuScalar output = empty;
    if (argument_count == 2U) {
      right_scalar =
          bennu_convert(bennu_project(right, index), parameter_type);
    }
    if (!bennu_kernel(resources, implementation, left_scalar, right_scalar,
                      &output)) {
      if (vector_result != 0) {
        bennu_release(resources, result);
      }
      return 0;
    }
    if (vector_result == 0) {
      if (output.type == BENNU_BOOL) {
        *result = bennu_scalar_bool(output.boolean);
      } else if (output.type == BENNU_INT) {
        *result = bennu_scalar_int(output.integer);
      } else {
        *result = bennu_scalar_double_bits(
            bennu_double_bits(output.double_precision));
      }
      return 1;
    }
    (void)bennu_store(result, index, output);
  }
  return 1;
}

static int bennu_write_text(const char *text) {
  return fputs(text, stdout) == EOF ? 0 : 1;
}

static int bennu_write_int(int64_t value) {
  return fprintf(stdout, "%" PRId64, value) < 0 ? 0 : 1;
}

static void bennu_normalize_exponent(const char *input, char *output,
                                     size_t capacity) {
  const char *exponent = strchr(input, 'e');
  const char *upper = strchr(input, 'E');
  size_t used = 0U;
  const char *digits = NULL;
  int negative = 0;
  if (exponent == NULL) {
    exponent = upper;
  }
  if (exponent == NULL) {
    (void)snprintf(output, capacity, "%s", input);
    return;
  }
  while (input != exponent && used + 1U < capacity) {
    output[used++] = *input++;
  }
  if (used + 1U < capacity) {
    output[used++] = 'e';
  }
  digits = exponent + 1;
  if (*digits == '+' || *digits == '-') {
    negative = *digits == '-';
    ++digits;
  }
  while (digits[0] == '0' && digits[1] != '\0') {
    ++digits;
  }
  if (negative != 0 && used + 1U < capacity) {
    output[used++] = '-';
  }
  while (*digits != '\0' && used + 1U < capacity) {
    output[used++] = *digits++;
  }
  output[used] = '\0';
}

static int bennu_write_double(double value) {
  const uint64_t bits = bennu_double_bits(value);
  const double magnitude = value < 0.0 ? -value : value;
  char candidate[64];
  char normalized[64];
  int precision = 0;
  int matched = 0;
  if ((bits & UINT64_C(0x7ff0000000000000)) ==
      UINT64_C(0x7ff0000000000000)) {
    if ((bits & UINT64_C(0x000fffffffffffff)) != UINT64_C(0)) {
      return bennu_write_text("nan");
    }
    return bennu_write_text((bits >> 63U) != 0U ? "-inf" : "inf");
  }
  if (bits == UINT64_C(0)) {
    return bennu_write_text("0.0");
  }
  if (bits == UINT64_C(0x8000000000000000)) {
    return bennu_write_text("-0.0");
  }
  candidate[0] = '\0';
  if (magnitude >= 1000000.0 || magnitude < 0.0001) {
    for (precision = 0; precision <= 16; ++precision) {
      char *end = NULL;
      double parsed = 0.0;
      if (snprintf(candidate, sizeof(candidate), "%.*e", precision, value) <
          0) {
        return 0;
      }
      parsed = strtod(candidate, &end);
      if (end != NULL && *end == '\0' &&
          bennu_double_bits(parsed) == bits) {
        matched = 1;
        break;
      }
    }
  } else {
    for (precision = 0; precision <= 20; ++precision) {
      char *end = NULL;
      double parsed = 0.0;
      if (snprintf(candidate, sizeof(candidate), "%.*f", precision, value) <
          0) {
        return 0;
      }
      parsed = strtod(candidate, &end);
      if (end != NULL && *end == '\0' &&
          bennu_double_bits(parsed) == bits) {
        matched = 1;
        break;
      }
    }
  }
  if (matched == 0) {
    return 0;
  }
  bennu_normalize_exponent(candidate, normalized, sizeof(normalized));
  if (strchr(normalized, '.') == NULL && strchr(normalized, 'e') == NULL) {
    if (!bennu_write_text(normalized)) {
      return 0;
    }
    return bennu_write_text(".0");
  }
  return bennu_write_text(normalized);
}

static int bennu_print_scalar(BennuType type, uint8_t boolean,
                              int64_t integer, double double_precision) {
  if (type == BENNU_BOOL) {
    return bennu_write_text(boolean != 0U ? "true" : "false");
  }
  if (type == BENNU_INT) {
    return bennu_write_int(integer);
  }
  return bennu_write_double(double_precision);
}

static int bennu_print_value(const BennuValue *value) {
  size_t index = 0U;
  if (value->container == BENNU_SCALAR) {
    if (!bennu_print_scalar(value->type, value->boolean, value->integer,
                            value->double_precision)) {
      return 0;
    }
    return bennu_write_text("\n");
  }
  if (!bennu_write_text("(")) {
    return 0;
  }
  for (index = 0U; index < value->count; ++index) {
    BennuScalar scalar = bennu_project(value, index);
    if (index != 0U && !bennu_write_text(" ")) {
      return 0;
    }
    if (!bennu_print_scalar(scalar.type, scalar.boolean, scalar.integer,
                            scalar.double_precision)) {
      return 0;
    }
  }
  return bennu_write_text(")\n");
}

static int bennu_report_failure(BennuFailure failure) {
  const char *message = "InternalError\n";
  if (failure == BENNU_FAILURE_SIZE) {
    message = "ResourceError: size_overflow\n";
  } else if (failure == BENNU_FAILURE_PROFILE) {
    message = "ResourceError: profile_limit\n";
  } else if (failure == BENNU_FAILURE_ALLOCATION) {
    message = "ResourceError: allocation_unavailable\n";
  } else if (failure == BENNU_FAILURE_DOMAIN) {
    message = "DomainError: integer_overflow\n";
  }
  return fputs(message, stderr) == EOF ? 0 : 1;
}

)bennu_c";
}

} // namespace bennu
