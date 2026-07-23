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

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <xmmintrin.h>
#endif
#endif

#ifndef BENNU_RUNTIME_MALLOC
#define BENNU_RUNTIME_MALLOC(size) malloc(size)
#endif

#ifndef BENNU_RUNTIME_FREE
#define BENNU_RUNTIME_FREE(data) free(data)
#endif

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
  BENNU_IMPL_NONE = 0,
  BENNU_IMPL_INC_INT = 1,
  BENNU_IMPL_INC_DOUBLE = 2,
  BENNU_IMPL_ADD_INT = 3,
  BENNU_IMPL_ADD_DOUBLE = 4,
  BENNU_IMPL_EQUALS_BOOL = 5,
  BENNU_IMPL_EQUALS_INT = 6,
  BENNU_IMPL_EQUALS_DOUBLE = 7,
  BENNU_IMPL_NOT_BOOL = 8,
  BENNU_IMPL_IOTA_INT = 9,
  BENNU_IMPL_DEC_INT = 10,
  BENNU_IMPL_DEC_DOUBLE = 11,
  BENNU_IMPL_NEG_INT = 12,
  BENNU_IMPL_NEG_DOUBLE = 13,
  BENNU_IMPL_ABS_INT = 14,
  BENNU_IMPL_ABS_DOUBLE = 15,
  BENNU_IMPL_SUB_INT = 16,
  BENNU_IMPL_SUB_DOUBLE = 17,
  BENNU_IMPL_MUL_INT = 18,
  BENNU_IMPL_MUL_DOUBLE = 19
} BennuImplementation;

typedef enum BennuPrimitiveId {
  BENNU_PRIMITIVE_NONE = -1,
  BENNU_PRIMITIVE_INC = 0,
  BENNU_PRIMITIVE_ADD = 1,
  BENNU_PRIMITIVE_EQUALS = 2,
  BENNU_PRIMITIVE_NOT = 3,
  BENNU_PRIMITIVE_IOTA = 4,
  BENNU_PRIMITIVE_DEC = 5,
  BENNU_PRIMITIVE_NEG = 6,
  BENNU_PRIMITIVE_ABS = 7,
  BENNU_PRIMITIVE_SUB = 8,
  BENNU_PRIMITIVE_MUL = 9
} BennuPrimitiveId;

typedef enum BennuFailure {
  BENNU_FAILURE_NONE = 0,
  BENNU_FAILURE_SIZE = 1,
  BENNU_FAILURE_PROFILE = 2,
  BENNU_FAILURE_ALLOCATION = 3,
  BENNU_FAILURE_DOMAIN = 4,
  BENNU_FAILURE_SHAPE = 5,
  BENNU_FAILURE_INTERNAL = 6
} BennuFailure;

typedef enum BennuProfile {
  BENNU_PROFILE_TRUSTED_LOCAL_V1 = 0,
  BENNU_PROFILE_BOUNDED_V1 = 1
} BennuProfile;

typedef enum BennuLimitKind {
  BENNU_LIMIT_NONE = 0,
  BENNU_LIMIT_MAX_VECTOR_BYTES = 1,
  BENNU_LIMIT_MAX_LIVE_EVALUATION_BYTES = 2,
  BENNU_LIMIT_MAX_WORK_UNITS = 3
} BennuLimitKind;

typedef struct BennuSourceLocation {
  size_t offset;
  size_t line;
  size_t column;
} BennuSourceLocation;

typedef struct BennuSourceSpan {
  BennuSourceLocation begin;
  BennuSourceLocation end;
} BennuSourceSpan;

typedef struct BennuScalarSignature {
  size_t parameter_count;
  BennuType parameter_types[2];
  BennuType result_type;
} BennuScalarSignature;

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
  BennuProfile profile;
  BennuLimitKind failure_limit;
  size_t failure_configured_limit;
  size_t failure_usage_before;
  size_t failure_refused_charge;
  const char *failure_admission_point;
  BennuSourceLocation failure_source_location;
  int failure_has_requested_elements;
  size_t failure_requested_elements;
  int failure_has_requested_bytes;
  size_t failure_requested_bytes;
  int failure_has_element_index;
  size_t failure_element_index;
  BennuImplementation failure_implementation;
  BennuScalar failure_left_operand;
  BennuScalar failure_right_operand;
  BennuPrimitiveId failure_primitive_id;
  BennuScalarSignature failure_signature;
  size_t failure_operand_count;
  BennuSourceSpan failure_primary_span;
  BennuSourceSpan failure_context_span;
} BennuResources;

static BennuSourceLocation bennu_source_location(size_t offset, size_t line,
                                                 size_t column) {
  BennuSourceLocation location = {offset, line, column};
  return location;
}

static BennuSourceSpan bennu_source_span(BennuSourceLocation begin,
                                         BennuSourceLocation end) {
  BennuSourceSpan span = {begin, end};
  return span;
}

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

static void bennu_set_resource_failure(
    BennuResources *resources, BennuFailure failure,
    int has_requested_elements, size_t requested_elements,
    int has_requested_bytes, size_t requested_bytes,
    const char *admission_point, BennuPrimitiveId primitive_id,
    BennuSourceSpan primary_span, BennuSourceSpan context_span) {
  if (resources->failure == BENNU_FAILURE_NONE) {
    resources->failure = failure;
    resources->failure_has_requested_elements = has_requested_elements;
    resources->failure_requested_elements = requested_elements;
    resources->failure_has_requested_bytes = has_requested_bytes;
    resources->failure_requested_bytes = requested_bytes;
    resources->failure_admission_point = admission_point;
    resources->failure_source_location = primary_span.begin;
    resources->failure_primitive_id = primitive_id;
    resources->failure_primary_span = primary_span;
    resources->failure_context_span = context_span;
  }
}

static void bennu_set_profile_failure(
    BennuResources *resources, BennuLimitKind limit_kind,
    size_t configured_limit, size_t usage_before, size_t refused_charge,
    int has_requested_elements, size_t requested_elements,
    int has_requested_bytes, size_t requested_bytes,
    const char *admission_point, BennuPrimitiveId primitive_id,
    BennuSourceSpan primary_span, BennuSourceSpan context_span) {
  if (resources->failure == BENNU_FAILURE_NONE) {
    resources->failure = BENNU_FAILURE_PROFILE;
    resources->failure_limit = limit_kind;
    resources->failure_configured_limit = configured_limit;
    resources->failure_usage_before = usage_before;
    resources->failure_refused_charge = refused_charge;
    resources->failure_has_requested_elements = has_requested_elements;
    resources->failure_requested_elements = requested_elements;
    resources->failure_has_requested_bytes = has_requested_bytes;
    resources->failure_requested_bytes = requested_bytes;
    resources->failure_admission_point = admission_point;
    resources->failure_source_location = primary_span.begin;
    resources->failure_primitive_id = primitive_id;
    resources->failure_primary_span = primary_span;
    resources->failure_context_span = context_span;
  }
}

static void bennu_set_domain_context(
    BennuResources *resources, BennuImplementation implementation,
    BennuScalar left, BennuScalar right, int has_element_index,
    size_t element_index, const char *admission_point,
    BennuPrimitiveId primitive_id, BennuScalarSignature signature,
    size_t operand_count, BennuSourceSpan primary_span,
    BennuSourceSpan context_span) {
  if (resources->failure == BENNU_FAILURE_DOMAIN &&
      resources->failure_admission_point == NULL) {
    resources->failure_implementation = implementation;
    resources->failure_left_operand = left;
    resources->failure_right_operand = right;
    resources->failure_primitive_id = primitive_id;
    resources->failure_signature = signature;
    resources->failure_operand_count = operand_count;
    resources->failure_has_element_index = has_element_index;
    resources->failure_element_index = element_index;
    resources->failure_admission_point = admission_point;
    resources->failure_source_location = primary_span.begin;
    resources->failure_primary_span = primary_span;
    resources->failure_context_span = context_span;
  }
}

static int bennu_require_shape(
    BennuResources *resources, const char *primitive,
    BennuPrimitiveId primitive_id, size_t argument_position,
    size_t expected_count, const BennuValue *argument,
    BennuSourceSpan primary_span, BennuSourceSpan context_span) {
  if (argument->container != BENNU_VECTOR) {
    bennu_set_failure(resources, BENNU_FAILURE_INTERNAL);
    return 0;
  }
  if (argument->count == expected_count) {
    return 1;
  }
  if (resources->failure == BENNU_FAILURE_NONE) {
    resources->failure = BENNU_FAILURE_SHAPE;
    resources->failure_configured_limit = expected_count;
    resources->failure_usage_before = argument->count;
    resources->failure_refused_charge = argument_position;
    resources->failure_admission_point = primitive;
    resources->failure_source_location = primary_span.begin;
    resources->failure_primitive_id = primitive_id;
    resources->failure_primary_span = primary_span;
    resources->failure_context_span = context_span;
  }
  return 0;
}

static int bennu_charge_work(
    BennuResources *resources, size_t work, const char *admission_point,
    BennuPrimitiveId primitive_id, BennuSourceSpan primary_span,
    BennuSourceSpan context_span) {
  if (work > SIZE_MAX - resources->work_units) {
    bennu_set_resource_failure(resources, BENNU_FAILURE_SIZE, 0, 0U, 0, 0U,
                               admission_point, primitive_id, primary_span,
                               context_span);
    return 0;
  }
  if (resources->has_work_limit != 0 &&
      resources->work_units + work > resources->work_limit) {
    bennu_set_profile_failure(
        resources, BENNU_LIMIT_MAX_WORK_UNITS, resources->work_limit,
        resources->work_units, work, 0, 0U, 0, 0U, admission_point,
        primitive_id, primary_span, context_span);
    return 0;
  }
  resources->work_units += work;
  return 1;
}

)bennu_c";
  source += R"bennu_c(static int bennu_allocate(BennuResources *resources, BennuValue *value,
                          BennuType type, size_t count, size_t work,
                          const char *admission_point,
                          BennuPrimitiveId primitive_id,
                          BennuSourceSpan primary_span,
                          BennuSourceSpan context_span) {
  const size_t width = bennu_width(type);
  size_t bytes = 0U;
  size_t live_after = 0U;
  size_t work_after = 0U;
  void *data = NULL;
  if (count > SIZE_MAX / width) {
    bennu_set_resource_failure(resources, BENNU_FAILURE_SIZE, 1, count, 0, 0U,
                               admission_point, primitive_id, primary_span,
                               context_span);
    return 0;
  }
  bytes = count * width;
  if (work > SIZE_MAX - resources->work_units ||
      bytes > SIZE_MAX - resources->live_bytes) {
    bennu_set_resource_failure(resources, BENNU_FAILURE_SIZE, 1, count, 1,
                               bytes, admission_point, primitive_id,
                               primary_span, context_span);
    return 0;
  }
  live_after = resources->live_bytes + bytes;
  work_after = resources->work_units + work;
  if (resources->has_vector_limit != 0 && bytes > resources->vector_limit) {
    bennu_set_profile_failure(
        resources, BENNU_LIMIT_MAX_VECTOR_BYTES, resources->vector_limit, 0U,
        bytes, 1, count, 1, bytes, admission_point, primitive_id, primary_span,
        context_span);
    return 0;
  }
  if (resources->has_live_limit != 0 && live_after > resources->live_limit) {
    bennu_set_profile_failure(
        resources, BENNU_LIMIT_MAX_LIVE_EVALUATION_BYTES,
        resources->live_limit, resources->live_bytes, bytes, 1, count, 1,
        bytes, admission_point, primitive_id, primary_span, context_span);
    return 0;
  }
  if (resources->has_work_limit != 0 && work_after > resources->work_limit) {
    bennu_set_profile_failure(
        resources, BENNU_LIMIT_MAX_WORK_UNITS, resources->work_limit,
        resources->work_units, work, 1, count, 1, bytes, admission_point,
        primitive_id, primary_span, context_span);
    return 0;
  }
  if (bytes != 0U) {
    const size_t ordinal = resources->reservation_ordinal;
    resources->reservation_ordinal += 1U;
    if (resources->has_failure_ordinal != 0 &&
        ordinal == resources->failure_ordinal) {
      bennu_set_resource_failure(resources, BENNU_FAILURE_ALLOCATION, 1, count,
                                 1, bytes, admission_point, primitive_id,
                                 primary_span, context_span);
      return 0;
    }
    data = BENNU_RUNTIME_MALLOC(bytes);
    if (data == NULL) {
      bennu_set_resource_failure(resources, BENNU_FAILURE_ALLOCATION, 1, count,
                                 1, bytes, admission_point, primitive_id,
                                 primary_span, context_span);
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
    BENNU_RUNTIME_FREE(value->data);
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
                              const uint8_t *values, size_t count,
                              const char *admission_point,
                              BennuSourceSpan primary_span,
                              BennuSourceSpan context_span) {
  if (!bennu_allocate(resources, result, BENNU_BOOL, count, 0U,
                      admission_point, BENNU_PRIMITIVE_NONE, primary_span,
                      context_span)) {
    return 0;
  }
  if (count != 0U) {
    (void)memcpy(result->data, values, count);
  }
  return 1;
}

static int bennu_literal_int(BennuResources *resources, BennuValue *result,
                             const int64_t *values, size_t count,
                             const char *admission_point,
                             BennuSourceSpan primary_span,
                             BennuSourceSpan context_span) {
  if (!bennu_allocate(resources, result, BENNU_INT, count, 0U,
                      admission_point, BENNU_PRIMITIVE_NONE, primary_span,
                      context_span)) {
    return 0;
  }
  if (count != 0U) {
    (void)memcpy(result->data, values, count * sizeof(int64_t));
  }
  return 1;
}

static int bennu_literal_double(BennuResources *resources,
                                BennuValue *result, const uint64_t *values,
                                size_t count, const char *admission_point,
                                BennuSourceSpan primary_span,
                                BennuSourceSpan context_span) {
  size_t index = 0U;
  double *output = NULL;
  if (!bennu_allocate(resources, result, BENNU_DOUBLE, count, 0U,
                      admission_point, BENNU_PRIMITIVE_NONE, primary_span,
                      context_span)) {
    return 0;
  }
  output = (double *)result->data;
  for (index = 0U; index < count; ++index) {
    output[index] = bennu_normalize_double(bennu_double_from_bits(values[index]));
  }
  return 1;
}

static int bennu_literal(BennuResources *resources, BennuValue *result,
                         BennuType type, const void *values, size_t count,
                         const char *admission_point,
                         BennuSourceSpan primary_span,
                         BennuSourceSpan context_span) {
  if (type == BENNU_BOOL) {
    return bennu_literal_bool(resources, result, (const uint8_t *)values,
                              count, admission_point, primary_span,
                              context_span);
  }
  if (type == BENNU_INT) {
    return bennu_literal_int(resources, result, (const int64_t *)values, count,
                             admission_point, primary_span, context_span);
  }
  return bennu_literal_double(resources, result, (const uint64_t *)values,
                              count, admission_point, primary_span,
                              context_span);
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
  const int negative = value < INT64_C(0);
  const uint64_t magnitude =
      negative != 0
          ? (uint64_t)(-(value + INT64_C(1))) + UINT64_C(1)
          : (uint64_t)value;
  uint64_t scan = magnitude;
  uint64_t significand = UINT64_C(0);
  uint64_t fraction = UINT64_C(0);
  uint64_t exponent = UINT64_C(0);
  unsigned int most_significant = 0U;
  if (magnitude == UINT64_C(0)) {
    return 0.0;
  }
  while (scan > UINT64_C(1)) {
    scan >>= 1U;
    ++most_significant;
  }
  if (most_significant <= 52U) {
    significand = magnitude << (52U - most_significant);
  } else {
    const unsigned int shift = most_significant - 52U;
    const uint64_t remainder_mask = (UINT64_C(1) << shift) - UINT64_C(1);
    const uint64_t remainder = magnitude & remainder_mask;
    const uint64_t halfway = UINT64_C(1) << (shift - 1U);
    significand = magnitude >> shift;
    if (remainder > halfway ||
        (remainder == halfway && (significand & UINT64_C(1)) != UINT64_C(0))) {
      ++significand;
      if (significand == (UINT64_C(1) << 53U)) {
        significand >>= 1U;
        ++most_significant;
      }
    }
  }
  exponent = (uint64_t)(most_significant + 1023U) << 52U;
  fraction = significand & UINT64_C(0x000fffffffffffff);
  return bennu_double_from_bits(
      (negative != 0 ? UINT64_C(0x8000000000000000) : UINT64_C(0)) |
      exponent | fraction);
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

static int bennu_sub_int(int64_t left, int64_t right, int64_t *result) {
  if ((right > INT64_C(0) && left < INT64_MIN + right) ||
      (right < INT64_C(0) && left > INT64_MAX + right)) {
    return 0;
  }
  *result = left - right;
  return 1;
}

static int bennu_mul_int(int64_t left, int64_t right, int64_t *result) {
  int overflow = 0;
  if (left > INT64_C(0)) {
    overflow = right > INT64_C(0) ? left > INT64_MAX / right
                                  : right < INT64_MIN / left;
  } else if (left < INT64_C(0)) {
    overflow = right > INT64_C(0)
                   ? left < INT64_MIN / right
                   : right < INT64_C(0) && right < INT64_MAX / left;
  }
  if (overflow != 0) {
    return 0;
  }
  *result = left * right;
  return 1;
}

typedef enum BennuDoubleOperation {
  BENNU_DOUBLE_ADD = 0,
  BENNU_DOUBLE_SUB = 1,
  BENNU_DOUBLE_MUL = 2
} BennuDoubleOperation;

static int bennu_double_is_nan(double value) {
  const uint64_t bits = bennu_double_bits(value);
  return (bits & UINT64_C(0x7ff0000000000000)) ==
             UINT64_C(0x7ff0000000000000) &&
         (bits & UINT64_C(0x000fffffffffffff)) != UINT64_C(0);
}

static int bennu_double_is_infinity(double value) {
  return (bennu_double_bits(value) & UINT64_C(0x7fffffffffffffff)) ==
         UINT64_C(0x7ff0000000000000);
}

static int bennu_double_is_zero(double value) {
  return (bennu_double_bits(value) & UINT64_C(0x7fffffffffffffff)) ==
         UINT64_C(0);
}

#if defined(__x86_64__) || defined(_M_X64)
typedef struct BennuStrictEnvironment {
  unsigned int control;
} BennuStrictEnvironment;

static void
bennu_begin_strict_environment(BennuStrictEnvironment *environment) {
  environment->control = _mm_getcsr();
  _mm_setcsr((environment->control | 0x1f80U) &
             ~(0x003fU | 0x0040U | 0x6000U | 0x8000U));
}

static void
bennu_restore_strict_environment(const BennuStrictEnvironment *environment) {
  _mm_setcsr(environment->control);
}
#elif defined(__aarch64__)
typedef struct BennuStrictEnvironment {
  uint64_t control;
  uint64_t status;
} BennuStrictEnvironment;

static void
bennu_begin_strict_environment(BennuStrictEnvironment *environment) {
  uint64_t strict_control = UINT64_C(0);
  const uint64_t clear_status = UINT64_C(0);
  __asm__ volatile("mrs %0, fpcr" : "=r"(environment->control));
  __asm__ volatile("mrs %0, fpsr" : "=r"(environment->status));
  strict_control = environment->control &
                   ~(UINT64_C(0x00009f00) | UINT64_C(0x00c00000) |
                     UINT64_C(0x03000000));
  __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(strict_control) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(clear_status) : "memory");
}

static void
bennu_restore_strict_environment(const BennuStrictEnvironment *environment) {
  __asm__ volatile("msr fpcr, %0\n\tisb"
                   :
                   : "r"(environment->control)
                   : "memory");
  __asm__ volatile("msr fpsr, %0"
                   :
                   : "r"(environment->status)
                   : "memory");
}
#else
#error "Bennu requires an x86-64 or AArch64 floating-point environment"
#endif

static double bennu_double_arithmetic(double left, double right,
                                      BennuDoubleOperation operation) {
  const int signs_differ =
      ((bennu_double_bits(left) ^ bennu_double_bits(right)) &
       UINT64_C(0x8000000000000000)) != UINT64_C(0);
  if (bennu_double_is_nan(left) != 0 || bennu_double_is_nan(right) != 0 ||
      (bennu_double_is_infinity(left) != 0 &&
       bennu_double_is_infinity(right) != 0 &&
       ((operation == BENNU_DOUBLE_ADD && signs_differ != 0) ||
        (operation == BENNU_DOUBLE_SUB && signs_differ == 0))) ||
      (operation == BENNU_DOUBLE_MUL &&
       ((bennu_double_is_infinity(left) != 0 &&
         bennu_double_is_zero(right) != 0) ||
        (bennu_double_is_zero(left) != 0 &&
         bennu_double_is_infinity(right) != 0)))) {
    return bennu_double_from_bits(UINT64_C(0x7ff8000000000000));
  }
  {
    BennuStrictEnvironment environment;
    volatile double volatile_left = left;
    volatile double volatile_right = right;
    volatile double result = 0.0;
    bennu_begin_strict_environment(&environment);
    if (operation == BENNU_DOUBLE_ADD) {
      result = volatile_left + volatile_right;
    } else if (operation == BENNU_DOUBLE_SUB) {
      result = volatile_left - volatile_right;
    } else {
      result = volatile_left * volatile_right;
    }
    bennu_restore_strict_environment(&environment);
    return bennu_normalize_double(result);
  }
}

static double bennu_add_double(double left, double right) {
  return bennu_double_arithmetic(left, right, BENNU_DOUBLE_ADD);
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
      implementation == BENNU_IMPL_ADD_DOUBLE ||
      implementation == BENNU_IMPL_DEC_DOUBLE ||
      implementation == BENNU_IMPL_NEG_DOUBLE ||
      implementation == BENNU_IMPL_ABS_DOUBLE ||
      implementation == BENNU_IMPL_SUB_DOUBLE ||
      implementation == BENNU_IMPL_MUL_DOUBLE) {
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
  } else if (implementation == BENNU_IMPL_DEC_INT) {
    if (left.integer == INT64_MIN) {
      bennu_set_failure(resources, BENNU_FAILURE_DOMAIN);
      return 0;
    }
    result->integer = left.integer - INT64_C(1);
  } else if (implementation == BENNU_IMPL_DEC_DOUBLE) {
    result->double_precision =
        bennu_double_arithmetic(left.double_precision, 1.0, BENNU_DOUBLE_SUB);
  } else if (implementation == BENNU_IMPL_NEG_INT) {
    if (left.integer == INT64_MIN) {
      bennu_set_failure(resources, BENNU_FAILURE_DOMAIN);
      return 0;
    }
    result->integer = -left.integer;
  } else if (implementation == BENNU_IMPL_NEG_DOUBLE) {
    result->double_precision = bennu_normalize_double(bennu_double_from_bits(
        bennu_double_bits(left.double_precision) ^
        UINT64_C(0x8000000000000000)));
  } else if (implementation == BENNU_IMPL_ABS_INT) {
    if (left.integer == INT64_MIN) {
      bennu_set_failure(resources, BENNU_FAILURE_DOMAIN);
      return 0;
    }
    result->integer = left.integer < INT64_C(0) ? -left.integer : left.integer;
  } else if (implementation == BENNU_IMPL_ABS_DOUBLE) {
    result->double_precision = bennu_normalize_double(bennu_double_from_bits(
        bennu_double_bits(left.double_precision) &
        UINT64_C(0x7fffffffffffffff)));
  } else if (implementation == BENNU_IMPL_SUB_INT) {
    if (!bennu_sub_int(left.integer, right.integer, &result->integer)) {
      bennu_set_failure(resources, BENNU_FAILURE_DOMAIN);
      return 0;
    }
  } else if (implementation == BENNU_IMPL_SUB_DOUBLE) {
    result->double_precision = bennu_double_arithmetic(
        left.double_precision, right.double_precision, BENNU_DOUBLE_SUB);
  } else if (implementation == BENNU_IMPL_MUL_INT) {
    if (!bennu_mul_int(left.integer, right.integer, &result->integer)) {
      bennu_set_failure(resources, BENNU_FAILURE_DOMAIN);
      return 0;
    }
  } else if (implementation == BENNU_IMPL_MUL_DOUBLE) {
    result->double_precision = bennu_double_arithmetic(
        left.double_precision, right.double_precision, BENNU_DOUBLE_MUL);
  } else {
    bennu_set_failure(resources, BENNU_FAILURE_INTERNAL);
    return 0;
  }
  return 1;
}

static int bennu_apply(BennuResources *resources,
                       BennuImplementation implementation,
                       BennuValue *result, const BennuValue *left,
                       const BennuValue *right, size_t argument_count,
                       const char *admission_point,
                       BennuPrimitiveId primitive_id,
                       BennuSourceSpan primary_span,
                       BennuSourceSpan context_span) {
  size_t count = 1U;
  size_t index = 0U;
  int vector_result = 0;
  BennuType parameter_type = BENNU_INT;
  BennuScalar empty = {BENNU_INT, 0U, INT64_C(0), 0.0};
  BennuScalarSignature signature = {
      0U, {BENNU_INT, BENNU_INT}, BENNU_INT};
  if (implementation == BENNU_IMPL_IOTA_INT) {
    int64_t bound = left->integer;
    if (bound > INT64_C(0)) {
      const uint64_t unsigned_bound = (uint64_t)bound;
      if (unsigned_bound > (uint64_t)SIZE_MAX) {
        bennu_set_resource_failure(resources, BENNU_FAILURE_SIZE, 0, 0U, 0,
                                   0U, admission_point, primitive_id,
                                   primary_span, context_span);
        return 0;
      }
      count = (size_t)unsigned_bound;
    } else {
      count = 0U;
    }
    if (!bennu_allocate(resources, result, BENNU_INT, count, count,
                        admission_point, primitive_id, primary_span,
                        context_span)) {
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
      implementation == BENNU_IMPL_DEC_DOUBLE ||
      implementation == BENNU_IMPL_NEG_DOUBLE ||
      implementation == BENNU_IMPL_ABS_DOUBLE ||
      implementation == BENNU_IMPL_SUB_DOUBLE ||
      implementation == BENNU_IMPL_MUL_DOUBLE ||
      implementation == BENNU_IMPL_EQUALS_DOUBLE) {
    parameter_type = BENNU_DOUBLE;
  } else if (implementation == BENNU_IMPL_EQUALS_BOOL ||
             implementation == BENNU_IMPL_NOT_BOOL) {
    parameter_type = BENNU_BOOL;
  }
  signature.parameter_count = argument_count;
  signature.parameter_types[0] = parameter_type;
  signature.parameter_types[1] = parameter_type;
  signature.result_type = bennu_result_type(implementation);
  if (vector_result != 0) {
    if (!bennu_allocate(resources, result, bennu_result_type(implementation),
                        count, count, admission_point, primitive_id,
                        primary_span, context_span)) {
      return 0;
    }
  } else if (!bennu_charge_work(resources, 1U, admission_point, primitive_id,
                                primary_span, context_span)) {
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
      bennu_set_domain_context(resources, implementation, left_scalar,
                               right_scalar, vector_result, index,
                               admission_point, primitive_id, signature,
                               argument_count, primary_span, context_span);
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

)bennu_c";
  source += R"bennu_c(static int bennu_print_scalar(BennuType type, uint8_t boolean,
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

static const char *bennu_profile_name(BennuProfile profile) {
  return profile == BENNU_PROFILE_BOUNDED_V1 ? "bounded-v1"
                                             : "trusted-local-v1";
}

static const char *bennu_limit_name(BennuLimitKind limit) {
  if (limit == BENNU_LIMIT_MAX_VECTOR_BYTES) {
    return "max_vector_bytes";
  }
  if (limit == BENNU_LIMIT_MAX_LIVE_EVALUATION_BYTES) {
    return "max_live_evaluation_bytes";
  }
  if (limit == BENNU_LIMIT_MAX_WORK_UNITS) {
    return "max_work_units";
  }
  return "none";
}

static int bennu_source_span_valid(BennuSourceSpan span) {
  return span.begin.offset != 0U && span.begin.line != 0U &&
         span.begin.column != 0U && span.end.offset >= span.begin.offset &&
         span.end.line != 0U && span.end.column != 0U;
}

static int bennu_failure_context_valid(const BennuResources *resources) {
  if (resources->failure_admission_point == NULL ||
      resources->failure_source_location.line == 0U ||
      resources->failure_source_location.column == 0U ||
      !bennu_source_span_valid(resources->failure_primary_span) ||
      !bennu_source_span_valid(resources->failure_context_span) ||
      resources->failure_source_location.offset !=
          resources->failure_primary_span.begin.offset ||
      resources->failure_source_location.line !=
          resources->failure_primary_span.begin.line ||
      resources->failure_source_location.column !=
          resources->failure_primary_span.begin.column) {
    return 0;
  }
  if (resources->failure == BENNU_FAILURE_PROFILE) {
    return resources->failure_limit != BENNU_LIMIT_NONE &&
           bennu_profile_name(resources->profile)[0] != '\0' &&
           bennu_limit_name(resources->failure_limit)[0] != '\0';
  }
  if (resources->failure == BENNU_FAILURE_SHAPE) {
    return resources->failure_primitive_id != BENNU_PRIMITIVE_NONE &&
           resources->failure_configured_limit !=
               resources->failure_usage_before &&
           resources->failure_refused_charge != 0U;
  }
  if (resources->failure == BENNU_FAILURE_ALLOCATION) {
    return resources->failure_has_requested_elements != 0 &&
           resources->failure_has_requested_bytes != 0;
  }
  if (resources->failure == BENNU_FAILURE_DOMAIN) {
    const int valid_inc =
        resources->failure_implementation == BENNU_IMPL_INC_INT &&
        resources->failure_primitive_id == BENNU_PRIMITIVE_INC &&
        resources->failure_signature.parameter_count == 1U &&
        resources->failure_operand_count == 1U;
    const int valid_add =
        resources->failure_implementation == BENNU_IMPL_ADD_INT &&
        resources->failure_primitive_id == BENNU_PRIMITIVE_ADD &&
        resources->failure_signature.parameter_count == 2U &&
        resources->failure_operand_count == 2U;
    const int valid_dec =
        resources->failure_implementation == BENNU_IMPL_DEC_INT &&
        resources->failure_primitive_id == BENNU_PRIMITIVE_DEC &&
        resources->failure_signature.parameter_count == 1U &&
        resources->failure_operand_count == 1U;
    const int valid_neg =
        resources->failure_implementation == BENNU_IMPL_NEG_INT &&
        resources->failure_primitive_id == BENNU_PRIMITIVE_NEG &&
        resources->failure_signature.parameter_count == 1U &&
        resources->failure_operand_count == 1U;
    const int valid_abs =
        resources->failure_implementation == BENNU_IMPL_ABS_INT &&
        resources->failure_primitive_id == BENNU_PRIMITIVE_ABS &&
        resources->failure_signature.parameter_count == 1U &&
        resources->failure_operand_count == 1U;
    const int valid_sub =
        resources->failure_implementation == BENNU_IMPL_SUB_INT &&
        resources->failure_primitive_id == BENNU_PRIMITIVE_SUB &&
        resources->failure_signature.parameter_count == 2U &&
        resources->failure_operand_count == 2U;
    const int valid_mul =
        resources->failure_implementation == BENNU_IMPL_MUL_INT &&
        resources->failure_primitive_id == BENNU_PRIMITIVE_MUL &&
        resources->failure_signature.parameter_count == 2U &&
        resources->failure_operand_count == 2U;
    return (valid_inc != 0 || valid_add != 0 || valid_dec != 0 ||
            valid_neg != 0 || valid_abs != 0 || valid_sub != 0 ||
            valid_mul != 0) &&
           resources->failure_signature.parameter_types[0] == BENNU_INT &&
           resources->failure_signature.result_type == BENNU_INT &&
           resources->failure_left_operand.type == BENNU_INT &&
           (resources->failure_operand_count == 1U ||
            (resources->failure_signature.parameter_types[1] == BENNU_INT &&
             resources->failure_right_operand.type == BENNU_INT));
  }
  return 1;
}

static int bennu_report_failure(const BennuResources *resources) {
  const char *reason = NULL;
  if (resources->failure == BENNU_FAILURE_PROFILE) {
    if (!bennu_failure_context_valid(resources)) {
      return fputs("InternalError\n", stderr) == EOF ? 0 : 1;
    }
    return fprintf(
               stderr,
               "bennu-source:%" PRIuMAX ":%" PRIuMAX
               ": ResourceError: reason=profile_limit profile=%s limit=%s "
               "configured=%" PRIuMAX " usage-before=%" PRIuMAX
               " refused-charge=%" PRIuMAX " admission=%s source=%" PRIuMAX
               ":%" PRIuMAX ":%" PRIuMAX "\n",
               (uintmax_t)resources->failure_source_location.line,
               (uintmax_t)resources->failure_source_location.column,
               bennu_profile_name(resources->profile),
               bennu_limit_name(resources->failure_limit),
               (uintmax_t)resources->failure_configured_limit,
               (uintmax_t)resources->failure_usage_before,
               (uintmax_t)resources->failure_refused_charge,
               resources->failure_admission_point,
               (uintmax_t)resources->failure_source_location.offset,
               (uintmax_t)resources->failure_source_location.line,
               (uintmax_t)resources->failure_source_location.column) < 0
               ? 0
               : 1;
  }
  if (resources->failure == BENNU_FAILURE_SIZE ||
      resources->failure == BENNU_FAILURE_ALLOCATION) {
    if (!bennu_failure_context_valid(resources)) {
      return fputs("InternalError\n", stderr) == EOF ? 0 : 1;
    }
    if (resources->failure == BENNU_FAILURE_SIZE) {
      reason = "size_overflow";
    } else {
      reason = "allocation_unavailable";
    }
    return fprintf(
               stderr,
               "bennu-source:%" PRIuMAX ":%" PRIuMAX
               ": ResourceError: %s resource request failed: %s\n",
               (uintmax_t)resources->failure_source_location.line,
               (uintmax_t)resources->failure_source_location.column,
               resources->failure_admission_point, reason) < 0
               ? 0
               : 1;
  }
  if (resources->failure == BENNU_FAILURE_DOMAIN) {
    if (!bennu_failure_context_valid(resources)) {
      return fputs("InternalError\n", stderr) == EOF ? 0 : 1;
    }
    if (resources->failure_has_element_index != 0) {
      return fprintf(
                 stderr,
                 "bennu-source:%" PRIuMAX ":%" PRIuMAX
                 ": DomainError: %s failed: integer_overflow at result index "
                 "%" PRIuMAX "\n",
                 (uintmax_t)resources->failure_source_location.line,
                 (uintmax_t)resources->failure_source_location.column,
                 resources->failure_admission_point,
                 (uintmax_t)resources->failure_element_index) < 0
                 ? 0
                 : 1;
    }
    return fprintf(
               stderr,
               "bennu-source:%" PRIuMAX ":%" PRIuMAX
               ": DomainError: %s failed: integer_overflow\n",
               (uintmax_t)resources->failure_source_location.line,
               (uintmax_t)resources->failure_source_location.column,
               resources->failure_admission_point) < 0
               ? 0
               : 1;
  }
  if (resources->failure == BENNU_FAILURE_SHAPE) {
    if (!bennu_failure_context_valid(resources)) {
      return fputs("InternalError\n", stderr) == EOF ? 0 : 1;
    }
    return fprintf(
               stderr,
               "bennu-source:%" PRIuMAX ":%" PRIuMAX
               ": ShapeMismatch: %s argument %" PRIuMAX
               " expected shape [%" PRIuMAX "], got [%" PRIuMAX "]\n",
               (uintmax_t)resources->failure_source_location.line,
               (uintmax_t)resources->failure_source_location.column,
               resources->failure_admission_point,
               (uintmax_t)resources->failure_refused_charge,
               (uintmax_t)resources->failure_configured_limit,
               (uintmax_t)resources->failure_usage_before) < 0
               ? 0
               : 1;
  }
  return fputs("InternalError\n", stderr) == EOF ? 0 : 1;
}

)bennu_c";
}

} // namespace bennu
