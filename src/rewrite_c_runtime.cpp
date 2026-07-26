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
  BENNU_VECTOR = 1,
  BENNU_TUPLE = 2
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
  BENNU_IMPL_AND_BOOL = 10,
  BENNU_IMPL_OR_BOOL = 11,
  BENNU_IMPL_NOT_EQUALS_BOOL = 12,
  BENNU_IMPL_NOT_EQUALS_INT = 13,
  BENNU_IMPL_NOT_EQUALS_DOUBLE = 14,
  BENNU_IMPL_ODD_INT = 15,
  BENNU_IMPL_EVEN_INT = 16,
  BENNU_IMPL_IS_POSITIVE_INT = 17,
  BENNU_IMPL_IS_POSITIVE_DOUBLE = 18,
  BENNU_IMPL_IS_NEGATIVE_INT = 19,
  BENNU_IMPL_IS_NEGATIVE_DOUBLE = 20,
  BENNU_IMPL_LESS_THAN_INT = 21,
  BENNU_IMPL_LESS_THAN_DOUBLE = 22,
  BENNU_IMPL_GREATER_THAN_INT = 23,
  BENNU_IMPL_GREATER_THAN_DOUBLE = 24
} BennuImplementation;

typedef enum BennuPrimitiveId {
  BENNU_PRIMITIVE_NONE = -1,
  BENNU_PRIMITIVE_INC = 0,
  BENNU_PRIMITIVE_ADD = 1,
  BENNU_PRIMITIVE_EQUALS = 2,
  BENNU_PRIMITIVE_NOT = 3,
  BENNU_PRIMITIVE_IOTA = 4,
  BENNU_PRIMITIVE_AND = 5,
  BENNU_PRIMITIVE_OR = 6,
  BENNU_PRIMITIVE_NOT_EQUALS = 7,
  BENNU_PRIMITIVE_ODD = 8,
  BENNU_PRIMITIVE_EVEN = 9,
  BENNU_PRIMITIVE_IS_POSITIVE = 10,
  BENNU_PRIMITIVE_IS_NEGATIVE = 11,
  BENNU_PRIMITIVE_LESS_THAN = 12,
  BENNU_PRIMITIVE_GREATER_THAN = 13
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
  BENNU_PROFILE_BOUNDED_V1 = 1,
  BENNU_PROFILE_TRUSTED_LOCAL_V2 = 2,
  BENNU_PROFILE_BOUNDED_V2 = 3
} BennuProfile;

typedef enum BennuLimitKind {
  BENNU_LIMIT_NONE = 0,
  BENNU_LIMIT_MAX_VECTOR_BYTES = 1,
  BENNU_LIMIT_MAX_LIVE_EVALUATION_BYTES = 2,
  BENNU_LIMIT_MAX_WORK_UNITS = 3,
  BENNU_LIMIT_MAX_TUPLE_TABLE_BYTES = 4
} BennuLimitKind;

typedef enum BennuArgumentDecode {
  BENNU_ARGUMENT_DECODE_OK = 0,
  BENNU_ARGUMENT_DECODE_INVALID_LITERAL = 1,
  BENNU_ARGUMENT_DECODE_OUT_OF_RANGE = 2
} BennuArgumentDecode;

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

typedef struct BennuValue BennuValue;

struct BennuValue {
  BennuContainer container;
  BennuType type;
  size_t count;
  uint8_t boolean;
  int64_t integer;
  double double_precision;
  void *data;
  BennuValue *parent;
  size_t parent_index;
  size_t cleanup_index;
};

typedef struct BennuResources {
  int has_vector_limit;
  int has_live_limit;
  int has_work_limit;
  int has_tuple_limit;
  size_t vector_limit;
  size_t live_limit;
  size_t work_limit;
  size_t tuple_limit;
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
  BennuSourceSpan failure_primitive_span;
  int failure_has_operand_span;
  BennuSourceSpan failure_operand_span;
  size_t failure_semantic_origin_count;
  BennuSourceSpan failure_semantic_origins[2];
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

static void bennu_prepare_spread_provenance(
    BennuResources *resources, BennuSourceSpan primitive_span,
    BennuSourceSpan operand_span, size_t semantic_origin_count,
    BennuSourceSpan first_origin, BennuSourceSpan second_origin) {
  if (resources->failure == BENNU_FAILURE_NONE) {
    resources->failure_primitive_span = primitive_span;
    resources->failure_has_operand_span = 1;
    resources->failure_operand_span = operand_span;
    resources->failure_semantic_origin_count = semantic_origin_count;
    resources->failure_semantic_origins[0] = first_origin;
    resources->failure_semantic_origins[1] = second_origin;
  }
}

static void bennu_clear_spread_provenance(BennuResources *resources) {
  BennuSourceSpan empty = {{0U, 0U, 0U}, {0U, 0U, 0U}};
  resources->failure_primitive_span = empty;
  resources->failure_has_operand_span = 0;
  resources->failure_operand_span = empty;
  resources->failure_semantic_origin_count = 0U;
  resources->failure_semantic_origins[0] = empty;
  resources->failure_semantic_origins[1] = empty;
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

static int bennu_require_spread_shape(
    BennuResources *resources, const char *primitive,
    BennuPrimitiveId primitive_id, size_t argument_position,
    size_t expected_count, const BennuValue *argument,
    BennuSourceSpan primary_span, BennuSourceSpan context_span,
    BennuSourceSpan primitive_span, BennuSourceSpan operand_span,
    size_t semantic_origin_count, BennuSourceSpan first_origin,
    BennuSourceSpan second_origin) {
  int accepted = 0;
  bennu_prepare_spread_provenance(
      resources, primitive_span, operand_span, semantic_origin_count,
      first_origin, second_origin);
  accepted = bennu_require_shape(
      resources, primitive, primitive_id, argument_position, expected_count,
      argument, primary_span, context_span);
  if (accepted != 0) {
    bennu_clear_spread_provenance(resources);
  }
  return accepted;
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

static int bennu_tuple(BennuResources *resources, BennuValue *result,
                       BennuValue **elements, size_t count,
                       const char *admission_point,
                       BennuSourceSpan primary_span,
                       BennuSourceSpan context_span) {
  const size_t slot_bytes = 16U;
  size_t bytes = 0U;
  size_t live_after = 0U;
  BennuValue *table = NULL;
  size_t index = 0U;
  if (count > SIZE_MAX / slot_bytes) {
    bennu_set_resource_failure(resources, BENNU_FAILURE_SIZE, 1, count, 0, 0U,
                               admission_point, BENNU_PRIMITIVE_NONE,
                               primary_span, context_span);
    return 0;
  }
  bytes = count * slot_bytes;
  if (bytes > SIZE_MAX - resources->live_bytes) {
    bennu_set_resource_failure(resources, BENNU_FAILURE_SIZE, 1, count, 1,
                               bytes, admission_point, BENNU_PRIMITIVE_NONE,
                               primary_span, context_span);
    return 0;
  }
  live_after = resources->live_bytes + bytes;
  if (resources->has_tuple_limit != 0 && bytes > resources->tuple_limit) {
    bennu_set_profile_failure(
        resources, BENNU_LIMIT_MAX_TUPLE_TABLE_BYTES, resources->tuple_limit,
        0U, bytes, 1, count, 1, bytes, admission_point,
        BENNU_PRIMITIVE_NONE, primary_span, context_span);
    return 0;
  }
  if (resources->has_live_limit != 0 && live_after > resources->live_limit) {
    bennu_set_profile_failure(
        resources, BENNU_LIMIT_MAX_LIVE_EVALUATION_BYTES,
        resources->live_limit, resources->live_bytes, bytes, 1, count, 1,
        bytes, admission_point, BENNU_PRIMITIVE_NONE, primary_span,
        context_span);
    return 0;
  }
  if (count != 0U) {
    const size_t ordinal = resources->reservation_ordinal;
    if (count > SIZE_MAX / sizeof(BennuValue)) {
      bennu_set_resource_failure(resources, BENNU_FAILURE_SIZE, 1, count, 1,
                                 bytes, admission_point,
                                 BENNU_PRIMITIVE_NONE, primary_span,
                                 context_span);
      return 0;
    }
    resources->reservation_ordinal += 1U;
    if (resources->has_failure_ordinal != 0 &&
        ordinal == resources->failure_ordinal) {
      bennu_set_resource_failure(resources, BENNU_FAILURE_ALLOCATION, 1, count,
                                 1, bytes, admission_point,
                                 BENNU_PRIMITIVE_NONE, primary_span,
                                 context_span);
      return 0;
    }
    table = (BennuValue *)BENNU_RUNTIME_MALLOC(count * sizeof(BennuValue));
    if (table == NULL) {
      bennu_set_resource_failure(resources, BENNU_FAILURE_ALLOCATION, 1, count,
                                 1, bytes, admission_point,
                                 BENNU_PRIMITIVE_NONE, primary_span,
                                 context_span);
      return 0;
    }
    (void)memset(table, 0, count * sizeof(BennuValue));
  }
  result->container = BENNU_TUPLE;
  result->count = count;
  result->data = table;
  result->cleanup_index = count;
  for (index = 0U; index < count; ++index) {
    BennuValue *child = elements[index];
    size_t nested_index = 0U;
    table[index] = *child;
    table[index].parent = result;
    table[index].parent_index = index;
    if (table[index].container == BENNU_TUPLE) {
      BennuValue *nested = (BennuValue *)table[index].data;
      for (nested_index = 0U; nested_index < table[index].count;
           ++nested_index) {
        nested[nested_index].parent = &table[index];
        nested[nested_index].parent_index = nested_index;
      }
    }
    (void)memset(child, 0, sizeof(*child));
  }
  resources->live_bytes = live_after;
  return 1;
}

static int bennu_fanout(BennuResources *resources, BennuValue *result,
                        BennuValue **empty_elements, size_t count,
                        const char *admission_point,
                        BennuSourceSpan primary_span,
                        BennuSourceSpan context_span) {
  if (!bennu_tuple(resources, result, empty_elements, count,
                   admission_point, primary_span, context_span)) {
    return 0;
  }
  result->cleanup_index = 0U;
  return 1;
}

static void bennu_tuple_transfer(BennuValue *result, size_t index,
                                 BennuValue *child) {
  BennuValue *table = (BennuValue *)result->data;
  size_t nested_index = 0U;
  table[index] = *child;
  table[index].parent = result;
  table[index].parent_index = index;
  if (table[index].container == BENNU_TUPLE) {
    BennuValue *nested = (BennuValue *)table[index].data;
    for (nested_index = 0U; nested_index < table[index].count;
         ++nested_index) {
      nested[nested_index].parent = &table[index];
      nested[nested_index].parent_index = nested_index;
    }
  }
  (void)memset(child, 0, sizeof(*child));
  result->cleanup_index += 1U;
}

static void bennu_release(BennuResources *resources, BennuValue *value) {
  BennuValue *current = value;
  while (current != NULL) {
    BennuValue *parent = NULL;
    if (current->container == BENNU_TUPLE &&
        current->cleanup_index != 0U) {
      BennuValue *children = (BennuValue *)current->data;
      current->cleanup_index -= 1U;
      current = &children[current->cleanup_index];
      continue;
    }
    parent = current->parent;
    if (current->container == BENNU_VECTOR) {
      const size_t bytes = current->count * bennu_width(current->type);
      if (bytes <= resources->live_bytes) {
        resources->live_bytes -= bytes;
      }
      BENNU_RUNTIME_FREE(current->data);
    } else if (current->container == BENNU_TUPLE) {
      const size_t bytes = current->count * 16U;
      if (bytes <= resources->live_bytes) {
        resources->live_bytes -= bytes;
      }
      BENNU_RUNTIME_FREE(current->data);
    }
    (void)memset(current, 0, sizeof(*current));
    current = parent;
  }
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

static int bennu_ascii_digit(char byte) {
  return byte >= '0' && byte <= '9';
}

static int bennu_canonical_integer_grammar(const char *spelling) {
  size_t index = 0U;
  if (spelling == NULL) {
    return 0;
  }
  if (spelling[index] == '-') {
    ++index;
  }
  if (spelling[index] == '\0') {
    return 0;
  }
  if (spelling[index] == '0') {
    return index == 0U && spelling[index + 1U] == '\0';
  }
  if (spelling[index] < '1' || spelling[index] > '9') {
    return 0;
  }
  do {
    ++index;
  } while (bennu_ascii_digit(spelling[index]));
  return spelling[index] == '\0';
}

static int bennu_finite_double_grammar(const char *spelling) {
  size_t index = 0U;
  size_t integer_begin = 0U;
  size_t fraction_begin = 0U;
  size_t exponent_begin = 0U;
  int has_fraction = 0;
  int has_exponent = 0;
  if (spelling == NULL) {
    return 0;
  }
  if (spelling[index] == '-') {
    ++index;
  }
  integer_begin = index;
  while (bennu_ascii_digit(spelling[index])) {
    ++index;
  }
  if (integer_begin == index ||
      (spelling[integer_begin] == '0' &&
       index != integer_begin + 1U)) {
    return 0;
  }
  if (spelling[index] == '.') {
    has_fraction = 1;
    ++index;
    fraction_begin = index;
    while (bennu_ascii_digit(spelling[index])) {
      ++index;
    }
    if (fraction_begin == index) {
      return 0;
    }
  }
  if (spelling[index] == 'e' || spelling[index] == 'E') {
    has_exponent = 1;
    ++index;
    if (spelling[index] == '+' || spelling[index] == '-') {
      ++index;
    }
    exponent_begin = index;
    while (bennu_ascii_digit(spelling[index])) {
      ++index;
    }
    if (exponent_begin == index) {
      return 0;
    }
  }
  return spelling[index] == '\0' &&
         (has_fraction != 0 || has_exponent != 0);
}
)bennu_c";
  source += R"bennu_c(
static BennuArgumentDecode bennu_decode_int_argument(
    const char *spelling, BennuValue *result) {
  const int negative = spelling != NULL && spelling[0] == '-';
  const size_t first_digit = negative != 0 ? 1U : 0U;
  const uint64_t limit =
      negative != 0
          ? UINT64_C(9223372036854775808)
          : UINT64_C(9223372036854775807);
  uint64_t magnitude = UINT64_C(0);
  size_t index = first_digit;
  if (!bennu_canonical_integer_grammar(spelling)) {
    return BENNU_ARGUMENT_DECODE_INVALID_LITERAL;
  }
  while (spelling[index] != '\0') {
    const uint64_t digit = (uint64_t)(spelling[index] - '0');
    if (magnitude > (limit - digit) / UINT64_C(10)) {
      return BENNU_ARGUMENT_DECODE_OUT_OF_RANGE;
    }
    magnitude = magnitude * UINT64_C(10) + digit;
    ++index;
  }
  if (negative != 0) {
    result->integer =
        magnitude == UINT64_C(9223372036854775808)
            ? INT64_MIN
            : -(int64_t)magnitude;
  } else {
    result->integer = (int64_t)magnitude;
  }
  *result = bennu_scalar_int(result->integer);
  return BENNU_ARGUMENT_DECODE_OK;
}

static BennuArgumentDecode bennu_decode_double_argument(
    const char *spelling, BennuValue *result) {
  char *end = NULL;
  double converted = 0.0;
  uint64_t bits = UINT64_C(0);
  if (spelling != NULL && strcmp(spelling, "inf") == 0) {
    *result = bennu_scalar_double_bits(UINT64_C(0x7ff0000000000000));
    return BENNU_ARGUMENT_DECODE_OK;
  }
  if (spelling != NULL && strcmp(spelling, "-inf") == 0) {
    *result = bennu_scalar_double_bits(UINT64_C(0xfff0000000000000));
    return BENNU_ARGUMENT_DECODE_OK;
  }
  if (spelling != NULL && strcmp(spelling, "nan") == 0) {
    *result = bennu_scalar_double_bits(UINT64_C(0x7ff8000000000000));
    return BENNU_ARGUMENT_DECODE_OK;
  }
  if (!bennu_finite_double_grammar(spelling)) {
    return BENNU_ARGUMENT_DECODE_INVALID_LITERAL;
  }
  converted = strtod(spelling, &end);
  bits = bennu_double_bits(converted);
  if (end == NULL || *end != '\0' ||
      (bits & UINT64_C(0x7ff0000000000000)) ==
          UINT64_C(0x7ff0000000000000)) {
    return BENNU_ARGUMENT_DECODE_OUT_OF_RANGE;
  }
  /*
   * A finite subnormal or signed zero is accepted. The supported C11
   * libraries perform one correctly rounded conversion; lexical
   * prevalidation prevents strtod extensions and partial parses.
   */
  *result = bennu_scalar_double_bits(bits);
  return BENNU_ARGUMENT_DECODE_OK;
}

static BennuArgumentDecode bennu_decode_argument(
    BennuType type, const char *spelling, BennuValue *result) {
  if (type == BENNU_BOOL) {
    if (spelling != NULL && strcmp(spelling, "true") == 0) {
      *result = bennu_scalar_bool(UINT8_C(1));
      return BENNU_ARGUMENT_DECODE_OK;
    }
    if (spelling != NULL && strcmp(spelling, "false") == 0) {
      *result = bennu_scalar_bool(UINT8_C(0));
      return BENNU_ARGUMENT_DECODE_OK;
    }
    return BENNU_ARGUMENT_DECODE_INVALID_LITERAL;
  }
  if (type == BENNU_INT) {
    return bennu_decode_int_argument(spelling, result);
  }
  return bennu_decode_double_argument(spelling, result);
}

static int bennu_report_argument_error(
    const char *reason, size_t required_count, size_t supplied_count,
    size_t position, const char *parameter_name, const char *expected_type,
    const BennuSourceSpan *declaration_span) {
  int written = 0;
  if (declaration_span == NULL) {
    written = fprintf(
        stderr,
        "bennu_argument_error reason=%s required_count=%" PRIuMAX
        " supplied_count=%" PRIuMAX " position=%" PRIuMAX
        " parameter_name=- expected_type=- declaration_span=-"
        " actual_container=- actual_type=-"
        " invalid_value_invariant=-\n",
        reason, (uintmax_t)required_count, (uintmax_t)supplied_count,
        (uintmax_t)position);
  } else {
    written = fprintf(
        stderr,
        "bennu_argument_error reason=%s required_count=%" PRIuMAX
        " supplied_count=%" PRIuMAX " position=%" PRIuMAX
        " parameter_name=%s expected_type=%s declaration_span=%" PRIuMAX
        ":%" PRIuMAX ":%" PRIuMAX "-%" PRIuMAX ":%" PRIuMAX ":%" PRIuMAX
        " actual_container=- actual_type=-"
        " invalid_value_invariant=-\n",
        reason, (uintmax_t)required_count, (uintmax_t)supplied_count,
        (uintmax_t)position, parameter_name, expected_type,
        (uintmax_t)declaration_span->begin.offset,
        (uintmax_t)declaration_span->begin.line,
        (uintmax_t)declaration_span->begin.column,
        (uintmax_t)declaration_span->end.offset,
        (uintmax_t)declaration_span->end.line,
        (uintmax_t)declaration_span->end.column);
  }
  return written < 0 ? 0 : 1;
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
      implementation == BENNU_IMPL_NOT_BOOL ||
      implementation == BENNU_IMPL_AND_BOOL ||
      implementation == BENNU_IMPL_OR_BOOL ||
      implementation == BENNU_IMPL_NOT_EQUALS_BOOL ||
      implementation == BENNU_IMPL_NOT_EQUALS_INT ||
      implementation == BENNU_IMPL_NOT_EQUALS_DOUBLE ||
      implementation == BENNU_IMPL_ODD_INT ||
      implementation == BENNU_IMPL_EVEN_INT ||
      implementation == BENNU_IMPL_IS_POSITIVE_INT ||
      implementation == BENNU_IMPL_IS_POSITIVE_DOUBLE ||
      implementation == BENNU_IMPL_IS_NEGATIVE_INT ||
      implementation == BENNU_IMPL_IS_NEGATIVE_DOUBLE ||
      implementation == BENNU_IMPL_LESS_THAN_INT ||
      implementation == BENNU_IMPL_LESS_THAN_DOUBLE ||
      implementation == BENNU_IMPL_GREATER_THAN_INT ||
      implementation == BENNU_IMPL_GREATER_THAN_DOUBLE) {
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
  } else if (implementation == BENNU_IMPL_AND_BOOL) {
    result->boolean = (uint8_t)(left.boolean & right.boolean);
  } else if (implementation == BENNU_IMPL_OR_BOOL) {
    result->boolean = (uint8_t)(left.boolean | right.boolean);
  } else if (implementation == BENNU_IMPL_NOT_EQUALS_BOOL) {
    result->boolean = (uint8_t)(left.boolean != right.boolean);
  } else if (implementation == BENNU_IMPL_NOT_EQUALS_INT) {
    result->boolean = (uint8_t)(left.integer != right.integer);
  } else if (implementation == BENNU_IMPL_NOT_EQUALS_DOUBLE) {
    result->boolean = (uint8_t)(left.double_precision != right.double_precision);
  } else if (implementation == BENNU_IMPL_ODD_INT) {
    result->boolean = (uint8_t)((left.integer % INT64_C(2)) != INT64_C(0));
  } else if (implementation == BENNU_IMPL_EVEN_INT) {
    result->boolean = (uint8_t)((left.integer % INT64_C(2)) == INT64_C(0));
  } else if (implementation == BENNU_IMPL_IS_POSITIVE_INT) {
    result->boolean = (uint8_t)(left.integer > INT64_C(0));
  } else if (implementation == BENNU_IMPL_IS_POSITIVE_DOUBLE) {
    result->boolean = (uint8_t)(left.double_precision > 0.0);
  } else if (implementation == BENNU_IMPL_IS_NEGATIVE_INT) {
    result->boolean = (uint8_t)(left.integer < INT64_C(0));
  } else if (implementation == BENNU_IMPL_IS_NEGATIVE_DOUBLE) {
    result->boolean = (uint8_t)(left.double_precision < 0.0);
  } else if (implementation == BENNU_IMPL_LESS_THAN_INT) {
    result->boolean = (uint8_t)(left.integer < right.integer);
  } else if (implementation == BENNU_IMPL_LESS_THAN_DOUBLE) {
    result->boolean = (uint8_t)(left.double_precision < right.double_precision);
  } else if (implementation == BENNU_IMPL_GREATER_THAN_INT) {
    result->boolean = (uint8_t)(left.integer > right.integer);
  } else if (implementation == BENNU_IMPL_GREATER_THAN_DOUBLE) {
    result->boolean = (uint8_t)(left.double_precision > right.double_precision);
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
      implementation == BENNU_IMPL_EQUALS_DOUBLE ||
      implementation == BENNU_IMPL_NOT_EQUALS_DOUBLE ||
      implementation == BENNU_IMPL_IS_POSITIVE_DOUBLE ||
      implementation == BENNU_IMPL_IS_NEGATIVE_DOUBLE ||
      implementation == BENNU_IMPL_LESS_THAN_DOUBLE ||
      implementation == BENNU_IMPL_GREATER_THAN_DOUBLE) {
    parameter_type = BENNU_DOUBLE;
  } else if (implementation == BENNU_IMPL_EQUALS_BOOL ||
             implementation == BENNU_IMPL_NOT_BOOL ||
             implementation == BENNU_IMPL_AND_BOOL ||
             implementation == BENNU_IMPL_OR_BOOL ||
             implementation == BENNU_IMPL_NOT_EQUALS_BOOL) {
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

static int bennu_apply_spread(
    BennuResources *resources, BennuImplementation implementation,
    BennuValue *result, const BennuValue *left, const BennuValue *right,
    size_t argument_count, const char *admission_point,
    BennuPrimitiveId primitive_id, BennuSourceSpan primary_span,
    BennuSourceSpan context_span, BennuSourceSpan primitive_span,
    BennuSourceSpan operand_span, size_t semantic_origin_count,
    BennuSourceSpan first_origin, BennuSourceSpan second_origin) {
  int applied = 0;
  bennu_prepare_spread_provenance(
      resources, primitive_span, operand_span, semantic_origin_count,
      first_origin, second_origin);
  applied = bennu_apply(
      resources, implementation, result, left, right, argument_count,
      admission_point, primitive_id, primary_span, context_span);
  if (applied != 0) {
    bennu_clear_spread_provenance(resources);
  }
  return applied;
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

static int bennu_value_valid(const BennuValue *value) {
  const BennuValue *current = value;
  if (value == NULL || value->parent != NULL) {
    return 0;
  }
  while (current != NULL) {
    if (current->container == BENNU_SCALAR) {
      if (current->type < BENNU_BOOL || current->type > BENNU_DOUBLE ||
          current->count != 1U || current->data != NULL ||
          current->cleanup_index != 0U) {
        return 0;
      }
    } else if (current->container == BENNU_VECTOR) {
      if (current->type < BENNU_BOOL || current->type > BENNU_DOUBLE ||
          (current->count != 0U && current->data == NULL) ||
          current->cleanup_index != 0U) {
        return 0;
      }
    } else if (current->container == BENNU_TUPLE) {
      const BennuValue *children = (const BennuValue *)current->data;
      size_t index = 0U;
      if ((current->count == 0U) != (current->data == NULL) ||
          current->cleanup_index != current->count) {
        return 0;
      }
      for (index = 0U; index < current->count; ++index) {
        if (children[index].parent != current ||
            children[index].parent_index != index) {
          return 0;
        }
      }
    } else {
      return 0;
    }
    if (current->container == BENNU_TUPLE && current->count != 0U) {
      current = &((const BennuValue *)current->data)[0];
      continue;
    }
    while (current->parent != NULL) {
      const BennuValue *parent = current->parent;
      const size_t next = current->parent_index + 1U;
      if (next < parent->count) {
        current = &((const BennuValue *)parent->data)[next];
        break;
      }
      current = parent;
    }
    if (current->parent == NULL) {
      return 1;
    }
  }
  return 0;
}

static int bennu_print_atom(const BennuValue *value) {
  size_t index = 0U;
  if (value->container == BENNU_SCALAR) {
    return bennu_print_scalar(value->type, value->boolean, value->integer,
                              value->double_precision);
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
  return bennu_write_text(")");
}

static int bennu_print_value(const BennuValue *value) {
  const BennuValue *current = value;
  if (!bennu_value_valid(value)) {
    return 0;
  }
  while (current != NULL) {
    if (current->container == BENNU_TUPLE) {
      if (!bennu_write_text("[")) {
        return 0;
      }
      if (current->count != 0U) {
        current = &((const BennuValue *)current->data)[0];
        continue;
      }
      if (!bennu_write_text("]")) {
        return 0;
      }
    } else if (!bennu_print_atom(current)) {
      return 0;
    }
    while (current->parent != NULL) {
      const BennuValue *parent = current->parent;
      const size_t next = current->parent_index + 1U;
      if (next < parent->count) {
        if (!bennu_write_text(" ")) {
          return 0;
        }
        current = &((const BennuValue *)parent->data)[next];
        break;
      }
      if (!bennu_write_text("]")) {
        return 0;
      }
      current = parent;
    }
    if (current->parent == NULL) {
      return bennu_write_text("\n");
    }
  }
  return 0;
}

static const char *bennu_profile_name(BennuProfile profile) {
  if (profile == BENNU_PROFILE_BOUNDED_V1) {
    return "bounded-v1";
  }
  if (profile == BENNU_PROFILE_TRUSTED_LOCAL_V2) {
    return "trusted-local-v2";
  }
  if (profile == BENNU_PROFILE_BOUNDED_V2) {
    return "bounded-v2";
  }
  return "trusted-local-v1";
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
  if (limit == BENNU_LIMIT_MAX_TUPLE_TABLE_BYTES) {
    return "max_tuple_table_bytes";
  }
  return "none";
}

static int bennu_source_span_valid(BennuSourceSpan span) {
  return span.begin.offset != 0U && span.begin.line != 0U &&
         span.begin.column != 0U && span.end.offset >= span.begin.offset &&
         span.end.line != 0U && span.end.column != 0U;
}

static int bennu_failure_context_valid(const BennuResources *resources) {
  size_t origin_index = 0U;
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
  if (resources->failure_has_operand_span != 0) {
    if (!bennu_source_span_valid(resources->failure_primitive_span) ||
        !bennu_source_span_valid(resources->failure_operand_span) ||
        resources->failure_semantic_origin_count > 2U) {
      return 0;
    }
    for (origin_index = 0U;
         origin_index < resources->failure_semantic_origin_count;
         ++origin_index) {
      if (!bennu_source_span_valid(
              resources->failure_semantic_origins[origin_index])) {
        return 0;
      }
    }
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
    return (valid_inc != 0 || valid_add != 0) &&
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
