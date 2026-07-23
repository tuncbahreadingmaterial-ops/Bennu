#include "bennu/c_emitter.hpp"
#include "bennu/native_builder.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool write_file(const char *path, std::string_view bytes) {
  std::FILE *file = std::fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written =
      std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size();
  return std::fclose(file) == 0 && written;
}

bool read_file(const char *path, std::string &bytes) {
  std::FILE *file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  char buffer[4096];
  while (true) {
    const std::size_t count = std::fread(buffer, 1U, sizeof(buffer), file);
    bytes.append(buffer, count);
    if (count != sizeof(buffer)) {
      const bool complete = std::feof(file) != 0 && std::ferror(file) == 0;
      return std::fclose(file) == 0 && complete;
    }
  }
}

void normalize_newlines(std::string &bytes) {
  std::size_t write = 0U;
  for (std::size_t read = 0U; read < bytes.size(); ++read) {
    if (bytes[read] == '\r' && read + 1U < bytes.size() &&
        bytes[read + 1U] == '\n') {
      continue;
    }
    bytes[write] = bytes[read];
    ++write;
  }
  bytes.resize(write);
}

void destroy_program(bennu::ProgramResult &result) {
  for (bennu::Value &value : result.values) {
    bennu::destroy_value(value);
  }
  result.values.clear();
}

bool is_resource_error(const bennu::Error &error,
                       bennu::ResourceErrorReason reason) {
  return error.kind == bennu::ErrorKind::resource_error &&
         error.resource.has_value() && error.resource->reason == reason;
}

bool emit_and_build(std::string_view source,
                    const bennu::EvaluationConfiguration &configuration,
                    const char *c_path, const char *native_path,
                    const char *compiler) {
  const bennu::CEmissionResult emitted =
      bennu::emit_c_source(source, configuration);
  if (!emitted.ok || emitted.source.empty() ||
      !write_file(c_path, emitted.source)) {
    return false;
  }
  const bennu::NativeBuildResult built = bennu::build_native(
      bennu::NativeBuildRequest{emitted.source, native_path, compiler, ""});
  return built.ok;
}

std::optional<std::string>
generated_failure_probe(std::string_view generated,
                        std::string_view failure_assertions) {
  if (generated.find("static int bennu_execute(BennuResources *snapshot)") ==
          std::string_view::npos ||
      generated.find("BENNU_RUNTIME_MALLOC") == std::string_view::npos ||
      generated.find("BENNU_RUNTIME_FREE") == std::string_view::npos) {
    return std::nullopt;
  }
  std::string probe = R"bennu_c(#include <stddef.h>
static void *bennu_probe_malloc(size_t size);
static void bennu_probe_free(void *data);
#define BENNU_RUNTIME_MALLOC(size) bennu_probe_malloc(size)
#define BENNU_RUNTIME_FREE(data) bennu_probe_free(data)
#define main bennu_generated_main
)bennu_c";
  probe += generated;
  probe += R"bennu_c(
#undef main

static size_t bennu_probe_outstanding_allocations = 0U;
static size_t bennu_probe_total_allocations = 0U;
static int bennu_probe_invalid_free = 0;

static void *bennu_probe_malloc(size_t size) {
  void *data = malloc(size);
  if (data != NULL) {
    bennu_probe_outstanding_allocations += 1U;
    bennu_probe_total_allocations += 1U;
  }
  return data;
}

static void bennu_probe_free(void *data) {
  if (data != NULL) {
    if (bennu_probe_outstanding_allocations == 0U) {
      bennu_probe_invalid_free = 1;
    } else {
      bennu_probe_outstanding_allocations -= 1U;
    }
  }
  free(data);
}

int main(void) {
  size_t iteration = 0U;
  for (iteration = 0U; iteration < 2U; ++iteration) {
    BennuResources snapshot = {0};
    const size_t allocations_before = bennu_probe_total_allocations;
    if (bennu_execute(&snapshot) == 0 || snapshot.live_bytes != 0U ||
        bennu_probe_outstanding_allocations != 0U ||
        bennu_probe_invalid_free != 0 ||
        snapshot.failure_admission_point == NULL ||
        !bennu_failure_context_valid(&snapshot) ||
)bennu_c";
  probe += failure_assertions;
  probe += R"bennu_c() {
      return 1;
    }
  }
  return 0;
}
)bennu_c";
  return probe;
}

bool emit_probe_and_build(
    std::string_view source,
    const bennu::EvaluationConfiguration &configuration, const char *c_path,
    const char *native_path, const char *compiler,
    std::string_view failure_assertions) {
  const bennu::CEmissionResult emitted =
      bennu::emit_c_source(source, configuration);
  if (!emitted.ok) {
    return false;
  }
  const std::optional<std::string> probe =
      generated_failure_probe(emitted.source, failure_assertions);
  if (!probe.has_value() || !write_file(c_path, *probe)) {
    return false;
  }
  const bennu::NativeBuildResult built = bennu::build_native(
      bennu::NativeBuildRequest{*probe, native_path, compiler, ""});
  return built.ok;
}

std::optional<std::string> generated_standard_fenv_probe() {
  const bennu::CEmissionResult emitted = bennu::emit_c_source(
      "dec[1.7976931348623157e308]\n"
      "sub[1.0 5.551115123125783e-17]\n"
      "mul[1.0000000000000002 1.0000000000000002]\n"
      "mul[5e-324 1.5]\n"
      "sub[1e-323 5e-324]\n");
  if (!emitted.ok ||
      emitted.source.find("static int bennu_execute(BennuResources *snapshot)") ==
          std::string::npos) {
    return std::nullopt;
  }
  std::string source = R"bennu_c(#include <fenv.h>
#define main bennu_generated_main
)bennu_c";
  source += emitted.source;
  source += R"bennu_c(
#undef main

int main(void) {
  static const int rounding_modes[] = {
      FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
  const int original_rounding = fegetround();
  size_t index = 0U;
  int result = 0;
#if defined(__x86_64__) || defined(_M_X64)
  const unsigned int original_control = _mm_getcsr();
#elif defined(__aarch64__)
  uint64_t original_control = UINT64_C(0);
  uint64_t original_status = UINT64_C(0);
  __asm__ volatile("mrs %0, fpcr" : "=r"(original_control));
  __asm__ volatile("mrs %0, fpsr" : "=r"(original_status));
#endif
  if (original_rounding == -1) {
    return 97;
  }
  for (index = 0U;
       index < sizeof(rounding_modes) / sizeof(rounding_modes[0]); ++index) {
    int caller_exceptions = 0;
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int caller_control = 0U;
#elif defined(__aarch64__)
    uint64_t caller_control = UINT64_C(0);
    uint64_t caller_status = UINT64_C(0);
#endif
    if (fesetround(rounding_modes[index]) != 0) {
      result = 96;
      break;
    }
    caller_exceptions = fetestexcept(FE_ALL_EXCEPT);
#if defined(__x86_64__) || defined(_M_X64)
    caller_control = _mm_getcsr();
#elif defined(__aarch64__)
    __asm__ volatile("mrs %0, fpcr" : "=r"(caller_control));
    __asm__ volatile("mrs %0, fpsr" : "=r"(caller_status));
#endif
    result = bennu_execute(NULL);
    if (result != 0 || fegetround() != rounding_modes[index] ||
        fetestexcept(FE_ALL_EXCEPT) != caller_exceptions) {
      if (result == 0) {
        result = 99;
      }
      break;
    }
#if defined(__x86_64__) || defined(_M_X64)
    if (_mm_getcsr() != caller_control) {
      result = 99;
      break;
    }
#elif defined(__aarch64__)
    {
      uint64_t restored_control = UINT64_C(0);
      uint64_t restored_status = UINT64_C(0);
      __asm__ volatile("mrs %0, fpcr" : "=r"(restored_control));
      __asm__ volatile("mrs %0, fpsr" : "=r"(restored_status));
      if (restored_control != caller_control || restored_status != caller_status) {
        result = 99;
        break;
      }
    }
#endif
  }
  (void)fesetround(original_rounding);
#if defined(__x86_64__) || defined(_M_X64)
  _mm_setcsr(original_control);
#elif defined(__aarch64__)
  __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(original_control) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(original_status) : "memory");
#endif
  return result;
}
)bennu_c";
  return source;
}

constexpr std::string_view allocation_iota_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_ALLOCATION ||
        snapshot.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_IOTA ||
        strcmp(snapshot.failure_admission_point, "iota") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != 2U ||
        snapshot.failure_has_requested_bytes == 0 ||
        snapshot.failure_requested_bytes != 16U ||
        snapshot.failure_primary_span.begin.line != 1U ||
        snapshot.failure_primary_span.begin.column != 1U ||
        bennu_probe_total_allocations != allocations_before)bennu_assert";

constexpr std::string_view allocation_lifted_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_ALLOCATION ||
        snapshot.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_INC ||
        strcmp(snapshot.failure_admission_point, "inc") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != 2U ||
        snapshot.failure_has_requested_bytes == 0 ||
        snapshot.failure_requested_bytes != 16U ||
        snapshot.failure_primary_span.begin.line != 1U ||
        snapshot.failure_primary_span.begin.column != 1U ||
        bennu_probe_total_allocations <= allocations_before)bennu_assert";

constexpr std::string_view allocation_late_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_ALLOCATION ||
        snapshot.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_IOTA ||
        strcmp(snapshot.failure_admission_point, "iota") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != 2U ||
        snapshot.failure_has_requested_bytes == 0 ||
        snapshot.failure_requested_bytes != 16U ||
        snapshot.failure_primary_span.begin.line != 2U ||
        snapshot.failure_primary_span.begin.column != 1U ||
        bennu_probe_total_allocations <= allocations_before)bennu_assert";

constexpr std::string_view work_refusal_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_PROFILE ||
        snapshot.profile != BENNU_PROFILE_BOUNDED_V1 ||
        snapshot.failure_limit != BENNU_LIMIT_MAX_WORK_UNITS ||
        snapshot.failure_configured_limit != 1U ||
        snapshot.failure_usage_before != 1U ||
        snapshot.failure_refused_charge != 1U ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_INC ||
        strcmp(snapshot.failure_admission_point, "inc") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != 1U ||
        snapshot.failure_has_requested_bytes == 0 ||
        snapshot.failure_requested_bytes != 8U ||
        snapshot.failure_primary_span.begin.line != 2U ||
        bennu_probe_total_allocations <= allocations_before)bennu_assert";

constexpr std::string_view vector_refusal_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_PROFILE ||
        snapshot.profile != BENNU_PROFILE_BOUNDED_V1 ||
        snapshot.failure_limit != BENNU_LIMIT_MAX_VECTOR_BYTES ||
        snapshot.failure_configured_limit != 8U ||
        snapshot.failure_usage_before != 0U ||
        snapshot.failure_refused_charge != 16U ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_IOTA ||
        strcmp(snapshot.failure_admission_point, "iota") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != 2U ||
        snapshot.failure_has_requested_bytes == 0 ||
        snapshot.failure_requested_bytes != 16U ||
        bennu_probe_total_allocations != allocations_before)bennu_assert";

constexpr std::string_view live_refusal_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_PROFILE ||
        snapshot.profile != BENNU_PROFILE_BOUNDED_V1 ||
        snapshot.failure_limit != BENNU_LIMIT_MAX_LIVE_EVALUATION_BYTES ||
        snapshot.failure_configured_limit != 8U ||
        snapshot.failure_usage_before != 8U ||
        snapshot.failure_refused_charge != 8U ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_INC ||
        strcmp(snapshot.failure_admission_point, "inc") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != 1U ||
        snapshot.failure_has_requested_bytes == 0 ||
        snapshot.failure_requested_bytes != 8U ||
        bennu_probe_total_allocations <= allocations_before)bennu_assert";

constexpr std::string_view size_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_SIZE ||
        snapshot.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_IOTA ||
        strcmp(snapshot.failure_admission_point, "iota") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != UINT64_C(2305843009213693952) ||
        snapshot.failure_has_requested_bytes != 0 ||
        bennu_probe_total_allocations != allocations_before)bennu_assert";

constexpr std::string_view shape_before_resource_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_SHAPE ||
        snapshot.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_ADD ||
        strcmp(snapshot.failure_admission_point, "add") != 0 ||
        snapshot.failure_configured_limit != 2U ||
        snapshot.failure_usage_before != 3U ||
        snapshot.failure_refused_charge != 1U ||
        snapshot.failure_has_requested_elements != 0 ||
        snapshot.failure_has_requested_bytes != 0 ||
        snapshot.failure_primary_span.begin.line != 1U ||
        snapshot.failure_primary_span.begin.column != 5U ||
        bennu_probe_total_allocations <= allocations_before)bennu_assert";

constexpr std::string_view resource_before_domain_assertions = R"bennu_assert(
        snapshot.failure != BENNU_FAILURE_ALLOCATION ||
        snapshot.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
        snapshot.failure_primitive_id != BENNU_PRIMITIVE_ADD ||
        strcmp(snapshot.failure_admission_point, "add") != 0 ||
        snapshot.failure_has_requested_elements == 0 ||
        snapshot.failure_requested_elements != 1U ||
        snapshot.failure_has_requested_bytes == 0 ||
        snapshot.failure_requested_bytes != 8U ||
        snapshot.failure_has_element_index != 0 ||
        snapshot.failure_primary_span.begin.line != 1U ||
        snapshot.failure_primary_span.begin.column != 1U ||
        bennu_probe_total_allocations <= allocations_before)bennu_assert";

std::optional<std::string> generated_runtime_probe() {
  const bennu::CEmissionResult emitted =
      bennu::emit_c_source(
          "1\nadd[(0 9223372036854775807) (0 1)]\n");
  if (!emitted.ok) {
    return std::nullopt;
  }
  constexpr std::string_view expected_domain_provenance =
      "\"add\", BENNU_PRIMITIVE_ADD, "
      "bennu_source_span(bennu_source_location(3U, 2U, 1U), "
      "bennu_source_location(6U, 2U, 4U)), "
      "bennu_source_span(bennu_source_location(3U, 2U, 1U), "
      "bennu_source_location(37U, 2U, 35U))";
  const bennu::CEmissionResult shape_emitted =
      bennu::emit_c_source("add[iota[2] iota[3]]\n");
  constexpr std::string_view expected_shape_provenance =
      "\"add\", BENNU_PRIMITIVE_ADD, 2U, bennu_values[1].count, "
      "&bennu_values[3], "
      "bennu_source_span(bennu_source_location(13U, 1U, 13U), "
      "bennu_source_location(20U, 1U, 20U)), "
      "bennu_source_span(bennu_source_location(1U, 1U, 1U), "
      "bennu_source_location(21U, 1U, 21U))";
  if (emitted.source.find(
          "static int bennu_execute(BennuResources *snapshot)") ==
          std::string::npos ||
      emitted.source.find("BENNU_RUNTIME_MALLOC") == std::string::npos ||
      emitted.source.find("BENNU_RUNTIME_FREE") == std::string::npos ||
      emitted.source.find(expected_domain_provenance) == std::string::npos ||
      !shape_emitted.ok ||
      shape_emitted.source.find(expected_shape_provenance) ==
          std::string::npos) {
    return std::nullopt;
  }
  std::string source = R"bennu_c(#include <stddef.h>
static void *bennu_probe_malloc(size_t size);
static void bennu_probe_free(void *data);
#define BENNU_RUNTIME_MALLOC(size) bennu_probe_malloc(size)
#define BENNU_RUNTIME_FREE(data) bennu_probe_free(data)
#define main bennu_generated_main
)bennu_c";
  source += emitted.source;
  source += R"bennu_c(

#undef main

static size_t bennu_probe_outstanding_allocations = 0U;
static size_t bennu_probe_total_allocations = 0U;
static size_t bennu_probe_total_frees = 0U;
static int bennu_probe_invalid_free = 0;

static void *bennu_probe_malloc(size_t size) {
  void *data = malloc(size);
  if (data != NULL) {
    bennu_probe_outstanding_allocations += 1U;
    bennu_probe_total_allocations += 1U;
  }
  return data;
}

static void bennu_probe_free(void *data) {
  if (data != NULL) {
    if (bennu_probe_outstanding_allocations == 0U) {
      bennu_probe_invalid_free = 1;
    } else {
      bennu_probe_outstanding_allocations -= 1U;
    }
    bennu_probe_total_frees += 1U;
  }
  free(data);
}

static int bennu_probe_domain_context(void) {
  BennuResources resources = {0};
  BennuValue input = {0};
  BennuValue result = {0};
  const int64_t values[] = {INT64_MAX};
  const BennuSourceSpan primary = bennu_source_span(
      bennu_source_location(5U, 1U, 5U),
      bennu_source_location(8U, 1U, 8U));
  const BennuSourceSpan context = bennu_source_span(
      bennu_source_location(1U, 1U, 1U),
      bennu_source_location(10U, 1U, 10U));
  resources.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
  if (!bennu_literal_int(&resources, &input, values, 1U, "vector-literal",
                         context, context)) {
    return 1;
  }
  if (bennu_apply(&resources, BENNU_IMPL_INC_INT, &result, &input, NULL, 1U,
                  "inc", BENNU_PRIMITIVE_INC, primary, context)) {
    return 2;
  }
  if (resources.failure != BENNU_FAILURE_DOMAIN ||
      resources.failure_implementation != BENNU_IMPL_INC_INT ||
      resources.failure_primitive_id != BENNU_PRIMITIVE_INC ||
      resources.failure_signature.parameter_count != 1U ||
      resources.failure_signature.parameter_types[0] != BENNU_INT ||
      resources.failure_signature.result_type != BENNU_INT ||
      resources.failure_operand_count != 1U ||
      resources.failure_left_operand.integer != INT64_MAX ||
      resources.failure_has_element_index == 0 ||
      resources.failure_element_index != 0U ||
      resources.failure_primary_span.begin.offset != 5U ||
      resources.failure_primary_span.end.offset != 8U ||
      resources.failure_context_span.begin.offset != 1U ||
      resources.failure_context_span.end.offset != 10U ||
      !bennu_failure_context_valid(&resources) || result.data != NULL ||
      resources.live_bytes != sizeof(int64_t)) {
    return 3;
  }
  bennu_release(&resources, &input);
  return resources.live_bytes == 0U ? 0 : 4;
}

static int bennu_probe_profile_contexts(void) {
  const BennuSourceSpan span = bennu_source_span(
      bennu_source_location(1U, 1U, 1U),
      bennu_source_location(8U, 1U, 8U));
  BennuResources resources = {0};
  BennuValue bound = bennu_scalar_int(INT64_C(2));
  BennuValue result = {0};
  resources.profile = BENNU_PROFILE_BOUNDED_V1;
  resources.has_vector_limit = 1;
  resources.vector_limit = 8U;
  if (bennu_apply(&resources, BENNU_IMPL_IOTA_INT, &result, &bound, NULL, 1U,
                  "iota", BENNU_PRIMITIVE_IOTA, span, span)) {
    return 1;
  }
  if (resources.failure != BENNU_FAILURE_PROFILE ||
      resources.failure_limit != BENNU_LIMIT_MAX_VECTOR_BYTES ||
      resources.failure_configured_limit != 8U ||
      resources.failure_usage_before != 0U ||
      resources.failure_refused_charge != 16U ||
      resources.failure_requested_elements != 2U ||
      resources.failure_requested_bytes != 16U ||
      resources.failure_primitive_id != BENNU_PRIMITIVE_IOTA ||
      !bennu_failure_context_valid(&resources) || resources.live_bytes != 0U) {
    return 2;
  }

  (void)memset(&resources, 0, sizeof(resources));
  resources.profile = BENNU_PROFILE_BOUNDED_V1;
  resources.has_work_limit = 1;
  if (bennu_apply(&resources, BENNU_IMPL_INC_INT, &result, &bound, NULL, 1U,
                  "inc", BENNU_PRIMITIVE_INC, span, span)) {
    return 3;
  }
  if (resources.failure != BENNU_FAILURE_PROFILE ||
      resources.failure_limit != BENNU_LIMIT_MAX_WORK_UNITS ||
      resources.failure_refused_charge != 1U ||
      resources.failure_has_requested_elements != 0 ||
      resources.failure_has_requested_bytes != 0 ||
      resources.failure_primitive_id != BENNU_PRIMITIVE_INC ||
      !bennu_failure_context_valid(&resources) || resources.live_bytes != 0U) {
    return 4;
  }

  (void)memset(&resources, 0, sizeof(resources));
  (void)memset(&result, 0, sizeof(result));
  resources.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
  resources.has_failure_ordinal = 1;
  if (bennu_apply(&resources, BENNU_IMPL_IOTA_INT, &result, &bound, NULL, 1U,
                  "iota", BENNU_PRIMITIVE_IOTA, span, span)) {
    return 5;
  }
  if (resources.failure != BENNU_FAILURE_ALLOCATION ||
      resources.failure_requested_elements != 2U ||
      resources.failure_requested_bytes != 16U ||
      resources.failure_primitive_id != BENNU_PRIMITIVE_IOTA ||
      !bennu_failure_context_valid(&resources) || resources.live_bytes != 0U) {
    return 6;
  }
  return 0;
}

static int bennu_probe_live_context(void) {
  const BennuSourceSpan span = bennu_source_span(
      bennu_source_location(1U, 1U, 1U),
      bennu_source_location(8U, 1U, 8U));
  const int64_t values[] = {INT64_C(1)};
  BennuResources resources = {0};
  BennuValue input = {0};
  BennuValue result = {0};
  resources.profile = BENNU_PROFILE_BOUNDED_V1;
  resources.has_live_limit = 1;
  resources.live_limit = 8U;
  if (!bennu_literal_int(&resources, &input, values, 1U, "vector-literal",
                         span, span)) {
    return 1;
  }
  if (bennu_apply(&resources, BENNU_IMPL_INC_INT, &result, &input, NULL, 1U,
                  "inc", BENNU_PRIMITIVE_INC, span, span)) {
    return 2;
  }
  if (resources.failure != BENNU_FAILURE_PROFILE ||
      resources.failure_limit != BENNU_LIMIT_MAX_LIVE_EVALUATION_BYTES ||
      resources.failure_configured_limit != 8U ||
      resources.failure_usage_before != 8U ||
      resources.failure_refused_charge != 8U ||
      resources.failure_requested_elements != 1U ||
      resources.failure_requested_bytes != 8U ||
      resources.failure_primitive_id != BENNU_PRIMITIVE_INC ||
      !bennu_failure_context_valid(&resources) || resources.live_bytes != 8U) {
    return 3;
  }
  bennu_release(&resources, &input);
  return resources.live_bytes == 0U ? 0 : 4;
}

static int bennu_probe_shape_context(void) {
  const BennuSourceSpan primary = bennu_source_span(
      bennu_source_location(13U, 1U, 13U),
      bennu_source_location(20U, 1U, 20U));
  const BennuSourceSpan context = bennu_source_span(
      bennu_source_location(1U, 1U, 1U),
      bennu_source_location(21U, 1U, 21U));
  BennuResources resources = {0};
  BennuValue argument = {0};
  resources.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
  argument.container = BENNU_VECTOR;
  argument.type = BENNU_INT;
  argument.count = 3U;
  if (bennu_require_shape(&resources, "add", BENNU_PRIMITIVE_ADD, 2U, 2U,
                          &argument, primary, context)) {
    return 1;
  }
  return resources.failure == BENNU_FAILURE_SHAPE &&
                 resources.failure_configured_limit == 2U &&
                 resources.failure_usage_before == 3U &&
                 resources.failure_refused_charge == 2U &&
                 resources.failure_primitive_id == BENNU_PRIMITIVE_ADD &&
                 resources.failure_primary_span.begin.offset == 13U &&
                 resources.failure_context_span.begin.offset == 1U &&
                 bennu_failure_context_valid(&resources)
             ? 0
             : 2;
}

typedef struct BennuCheckedDoubleCase {
  BennuImplementation implementation;
  uint64_t left_bits;
  uint64_t right_bits;
  uint64_t expected_bits;
} BennuCheckedDoubleCase;

static int bennu_probe_checked_double_bits(void) {
  static const BennuCheckedDoubleCase cases[] = {
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x0000000000000000), UINT64_C(0), UINT64_C(0xbff0000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x8000000000000000), UINT64_C(0), UINT64_C(0xbff0000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x0000000000000001), UINT64_C(0), UINT64_C(0xbff0000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x000fffffffffffff), UINT64_C(0), UINT64_C(0xbff0000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x0010000000000000), UINT64_C(0), UINT64_C(0xbff0000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x7fefffffffffffff), UINT64_C(0), UINT64_C(0x7fefffffffffffff)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x7ff0000000000000), UINT64_C(0), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0xfff0000000000000), UINT64_C(0), UINT64_C(0xfff0000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x7ff8123456789abc), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0xfff8123456789abc), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x7ff0000000000001), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0xfff0000000000001), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x7ff8000000000000), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x4340000000000001), UINT64_C(0), UINT64_C(0x4340000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x0000000000000000), UINT64_C(0), UINT64_C(0x8000000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x8000000000000000), UINT64_C(0), UINT64_C(0x0000000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x0000000000000001), UINT64_C(0), UINT64_C(0x8000000000000001)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x000fffffffffffff), UINT64_C(0), UINT64_C(0x800fffffffffffff)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x0010000000000000), UINT64_C(0), UINT64_C(0x8010000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x7fefffffffffffff), UINT64_C(0), UINT64_C(0xffefffffffffffff)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x7ff0000000000000), UINT64_C(0), UINT64_C(0xfff0000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0xfff0000000000000), UINT64_C(0), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x7ff8123456789abc), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0xfff0000000000001), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x7ff8000000000000), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x0000000000000000), UINT64_C(0), UINT64_C(0x0000000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x8000000000000000), UINT64_C(0), UINT64_C(0x0000000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x8000000000000001), UINT64_C(0), UINT64_C(0x0000000000000001)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x800fffffffffffff), UINT64_C(0), UINT64_C(0x000fffffffffffff)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x8010000000000000), UINT64_C(0), UINT64_C(0x0010000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0xffefffffffffffff), UINT64_C(0), UINT64_C(0x7fefffffffffffff)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x7ff0000000000000), UINT64_C(0), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0xfff0000000000000), UINT64_C(0), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x7ff0000000000001), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0xfff8123456789abc), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x7ff8000000000000), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x0010000000000000), UINT64_C(0x000fffffffffffff), UINT64_C(0x0000000000000001)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x000fffffffffffff), UINT64_C(0), UINT64_C(0x000fffffffffffff)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x0010000000000000), UINT64_C(0), UINT64_C(0x0010000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x7fefffffffffffff), UINT64_C(0), UINT64_C(0x7fefffffffffffff)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x7ff0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0xfff0000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0xfff0000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x3ff0000000000000), UINT64_C(0x3c90000000000000), UINT64_C(0x3ff0000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x7fefffffffffffff), UINT64_C(0xffefffffffffffff), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x7ff8123456789abc), UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x3ff0000000000000), UINT64_C(0xfff0000000000001), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x7ff8000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x8000000000000000), UINT64_C(0x4000000000000000), UINT64_C(0x8000000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x0000000000000001), UINT64_C(0x4000000000000000), UINT64_C(0x0000000000000002)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x000fffffffffffff), UINT64_C(0x3ff0000000000000), UINT64_C(0x000fffffffffffff)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x0010000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x0010000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x7fefffffffffffff), UINT64_C(0x3ff0000000000000), UINT64_C(0x7fefffffffffffff)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x0000000000000001), UINT64_C(0x3fe0000000000000), UINT64_C(0x0000000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x8000000000000001), UINT64_C(0x3fe0000000000000), UINT64_C(0x8000000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x0000000000000001), UINT64_C(0x3ff8000000000000), UINT64_C(0x0000000000000002)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x7fefffffffffffff), UINT64_C(0x4000000000000000), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x7ff0000000000000), UINT64_C(0), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x7ff8123456789abc), UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x3ff0000000000000), UINT64_C(0xfff0000000000001), UINT64_C(0x7ff8000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x7ff8000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff8000000000000)}};
  size_t index = 0U;
  for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    BennuResources resources = {0};
    BennuScalar left = {BENNU_DOUBLE, 0U, INT64_C(0),
                        bennu_double_from_bits(cases[index].left_bits)};
    BennuScalar right = {BENNU_DOUBLE, 0U, INT64_C(0),
                         bennu_double_from_bits(cases[index].right_bits)};
    BennuScalar result = {0};
    if (!bennu_kernel(&resources, cases[index].implementation, left, right,
                      &result) ||
        resources.failure != BENNU_FAILURE_NONE ||
        result.type != BENNU_DOUBLE ||
        bennu_double_bits(result.double_precision) != cases[index].expected_bits) {
      return 1;
    }
  }
  return 0;
}
)bennu_c";
  source += R"bennu_c(

static int bennu_probe_hostile_checked_double_environment(void) {
  static const BennuCheckedDoubleCase cases[] = {
      {BENNU_IMPL_DEC_DOUBLE, UINT64_C(0x4340000000000001), UINT64_C(0), UINT64_C(0x4340000000000000)},
      {BENNU_IMPL_NEG_DOUBLE, UINT64_C(0x0000000000000001), UINT64_C(0), UINT64_C(0x8000000000000001)},
      {BENNU_IMPL_ABS_DOUBLE, UINT64_C(0x8000000000000001), UINT64_C(0), UINT64_C(0x0000000000000001)},
      {BENNU_IMPL_SUB_DOUBLE, UINT64_C(0x3ff0000000000000), UINT64_C(0x3c90000000000000), UINT64_C(0x3ff0000000000000)},
      {BENNU_IMPL_MUL_DOUBLE, UINT64_C(0x0000000000000001), UINT64_C(0x3ff8000000000000), UINT64_C(0x0000000000000002)}};
  size_t round_index = 0U;
  size_t case_index = 0U;
#if defined(__x86_64__) || defined(_M_X64)
  static const unsigned int rounds[] = {0x2000U, 0x4000U, 0x6000U};
  const unsigned int original = _mm_getcsr();
  for (round_index = 0U; round_index < sizeof(rounds) / sizeof(rounds[0]); ++round_index) {
    const unsigned int hostile =
        (original & ~(0x003fU | 0x6000U)) | 0x0021U | 0x0040U | 0x8000U |
        rounds[round_index];
    _mm_setcsr(hostile);
    for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
      BennuResources resources = {0};
      BennuScalar left = {BENNU_DOUBLE, 0U, INT64_C(0), bennu_double_from_bits(cases[case_index].left_bits)};
      BennuScalar right = {BENNU_DOUBLE, 0U, INT64_C(0), bennu_double_from_bits(cases[case_index].right_bits)};
      BennuScalar result = {0};
      if (!bennu_kernel(&resources, cases[case_index].implementation, left, right, &result) ||
          bennu_double_bits(result.double_precision) != cases[case_index].expected_bits ||
          _mm_getcsr() != hostile) {
        _mm_setcsr(original);
        return 1;
      }
    }
  }
  {
    const unsigned int enabled_overflow_trap =
        (original & ~(0x003fU | 0x6000U | 0x0400U)) |
        0x0021U | (0x1f80U & ~0x0400U) | 0x0040U | 0x8000U | 0x2000U;
    BennuResources resources = {0};
    BennuScalar left = {BENNU_DOUBLE, 0U, INT64_C(0), bennu_double_from_bits(UINT64_C(0x7fefffffffffffff))};
    BennuScalar right = {BENNU_DOUBLE, 0U, INT64_C(0), 2.0};
    BennuScalar result = {0};
    _mm_setcsr(enabled_overflow_trap);
    if (!bennu_kernel(&resources, BENNU_IMPL_MUL_DOUBLE, left, right, &result) ||
        bennu_double_bits(result.double_precision) != UINT64_C(0x7ff0000000000000) ||
        _mm_getcsr() != enabled_overflow_trap) {
      _mm_setcsr(original);
      return 2;
    }
  }
  _mm_setcsr(original);
#elif defined(__aarch64__)
  static const uint64_t rounds[] = {UINT64_C(0x00400000), UINT64_C(0x00800000), UINT64_C(0x00c00000)};
  uint64_t original_control = UINT64_C(0);
  uint64_t original_status = UINT64_C(0);
  uint64_t hostile_status = UINT64_C(0);
  __asm__ volatile("mrs %0, fpcr" : "=r"(original_control));
  __asm__ volatile("mrs %0, fpsr" : "=r"(original_status));
  hostile_status = original_status | UINT64_C(0x00000011);
  __asm__ volatile("msr fpsr, %0" : : "r"(hostile_status) : "memory");
  for (round_index = 0U; round_index < sizeof(rounds) / sizeof(rounds[0]); ++round_index) {
    const uint64_t hostile = (original_control & ~UINT64_C(0x00c00000)) |
                             UINT64_C(0x01000000) | rounds[round_index];
    __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(hostile) : "memory");
    for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
      BennuResources resources = {0};
      BennuScalar left = {BENNU_DOUBLE, 0U, INT64_C(0), bennu_double_from_bits(cases[case_index].left_bits)};
      BennuScalar right = {BENNU_DOUBLE, 0U, INT64_C(0), bennu_double_from_bits(cases[case_index].right_bits)};
      BennuScalar result = {0};
      uint64_t restored_control = UINT64_C(0);
      uint64_t restored_status = UINT64_C(0);
      if (!bennu_kernel(&resources, cases[case_index].implementation, left, right, &result) ||
          bennu_double_bits(result.double_precision) != cases[case_index].expected_bits) {
        __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(original_control) : "memory");
        __asm__ volatile("msr fpsr, %0" : : "r"(original_status) : "memory");
        return 1;
      }
      __asm__ volatile("mrs %0, fpcr" : "=r"(restored_control));
      __asm__ volatile("mrs %0, fpsr" : "=r"(restored_status));
      if (restored_control != hostile || restored_status != hostile_status) {
        __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(original_control) : "memory");
        __asm__ volatile("msr fpsr, %0" : : "r"(original_status) : "memory");
        return 2;
      }
    }
  }
  {
    const uint64_t enabled_overflow_trap =
        (original_control & ~(UINT64_C(0x00c00000) | UINT64_C(0x00009f00))) |
        UINT64_C(0x01000000) | UINT64_C(0x00800000) | UINT64_C(0x00000400);
    BennuResources resources = {0};
    BennuScalar left = {BENNU_DOUBLE, 0U, INT64_C(0), bennu_double_from_bits(UINT64_C(0x7fefffffffffffff))};
    BennuScalar right = {BENNU_DOUBLE, 0U, INT64_C(0), 2.0};
    BennuScalar result = {0};
    uint64_t restored_control = UINT64_C(0);
    uint64_t restored_status = UINT64_C(0);
    __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(enabled_overflow_trap) : "memory");
    __asm__ volatile("msr fpsr, %0" : : "r"(hostile_status) : "memory");
    if (!bennu_kernel(&resources, BENNU_IMPL_MUL_DOUBLE, left, right, &result)) {
      __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(original_control) : "memory");
      __asm__ volatile("msr fpsr, %0" : : "r"(original_status) : "memory");
      return 3;
    }
    __asm__ volatile("mrs %0, fpcr" : "=r"(restored_control));
    __asm__ volatile("mrs %0, fpsr" : "=r"(restored_status));
    if (restored_control != enabled_overflow_trap || restored_status != hostile_status ||
        bennu_double_bits(result.double_precision) != UINT64_C(0x7ff0000000000000)) {
      __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(original_control) : "memory");
      __asm__ volatile("msr fpsr, %0" : : "r"(original_status) : "memory");
      return 4;
    }
  }
  __asm__ volatile("msr fpcr, %0\n\tisb" : : "r"(original_control) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(original_status) : "memory");
#else
  return 5;
#endif
  return 0;
}

typedef struct BennuCheckedIntCase {
  const char *name;
  BennuImplementation implementation;
  BennuPrimitiveId primitive_id;
  size_t arity;
  int64_t right;
} BennuCheckedIntCase;

static int bennu_probe_checked_int_contexts(void) {
  static const BennuCheckedIntCase cases[] = {
      {"dec", BENNU_IMPL_DEC_INT, BENNU_PRIMITIVE_DEC, 1U, INT64_C(0)},
      {"neg", BENNU_IMPL_NEG_INT, BENNU_PRIMITIVE_NEG, 1U, INT64_C(0)},
      {"abs", BENNU_IMPL_ABS_INT, BENNU_PRIMITIVE_ABS, 1U, INT64_C(0)},
      {"sub", BENNU_IMPL_SUB_INT, BENNU_PRIMITIVE_SUB, 2U, INT64_C(1)},
      {"mul", BENNU_IMPL_MUL_INT, BENNU_PRIMITIVE_MUL, 2U, -INT64_C(1)}};
  const BennuSourceSpan primary = bennu_source_span(
      bennu_source_location(7U, 3U, 2U), bennu_source_location(11U, 3U, 6U));
  const BennuSourceSpan context = bennu_source_span(
      bennu_source_location(5U, 3U, 1U), bennu_source_location(40U, 3U, 36U));
  size_t case_index = 0U;
  for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
    size_t iteration = 0U;
    for (iteration = 0U; iteration < 2U; ++iteration) {
      BennuResources scalar_resources = {0};
      BennuValue scalar_left = bennu_scalar_int(INT64_MIN);
      BennuValue scalar_right = bennu_scalar_int(cases[case_index].right);
      BennuValue scalar_result = {0};
      scalar_resources.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
      if (bennu_apply(&scalar_resources, cases[case_index].implementation,
                      &scalar_result, &scalar_left, &scalar_right,
                      cases[case_index].arity, cases[case_index].name,
                      cases[case_index].primitive_id, primary, context) ||
          scalar_resources.failure != BENNU_FAILURE_DOMAIN ||
          scalar_resources.failure_implementation != cases[case_index].implementation ||
          scalar_resources.failure_primitive_id != cases[case_index].primitive_id ||
          scalar_resources.failure_domain_reason !=
              BENNU_DOMAIN_INTEGER_OVERFLOW ||
          strcmp(scalar_resources.failure_admission_point, cases[case_index].name) != 0 ||
          scalar_resources.failure_signature.parameter_count != cases[case_index].arity ||
          scalar_resources.failure_signature.parameter_types[0] != BENNU_INT ||
          scalar_resources.failure_signature.result_type != BENNU_INT ||
          scalar_resources.failure_operand_count != cases[case_index].arity ||
          scalar_resources.failure_left_operand.type != BENNU_INT ||
          scalar_resources.failure_left_operand.integer != INT64_MIN ||
          (cases[case_index].arity == 2U &&
           (scalar_resources.failure_signature.parameter_types[1] != BENNU_INT ||
            scalar_resources.failure_right_operand.type != BENNU_INT ||
            scalar_resources.failure_right_operand.integer != cases[case_index].right)) ||
          scalar_resources.failure_has_element_index != 0 ||
          scalar_resources.failure_primary_span.begin.offset != 7U ||
          scalar_resources.failure_primary_span.begin.line != 3U ||
          scalar_resources.failure_primary_span.begin.column != 2U ||
          scalar_resources.failure_primary_span.end.offset != 11U ||
          scalar_resources.failure_primary_span.end.line != 3U ||
          scalar_resources.failure_primary_span.end.column != 6U ||
          scalar_resources.failure_context_span.begin.offset != 5U ||
          scalar_resources.failure_context_span.begin.line != 3U ||
          scalar_resources.failure_context_span.begin.column != 1U ||
          scalar_resources.failure_context_span.end.offset != 40U ||
          scalar_resources.failure_context_span.end.line != 3U ||
          scalar_resources.failure_context_span.end.column != 36U ||
          scalar_resources.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
          scalar_resources.work_units != 1U ||
          scalar_resources.live_bytes != 0U ||
          scalar_resources.reservation_ordinal != 0U ||
          scalar_result.data != NULL ||
          !bennu_failure_context_valid(&scalar_resources)) {
        return 1;
      }
      {
        BennuResources lifted_resources = {0};
        BennuValue lifted_left = {0};
        BennuValue lifted_right = bennu_scalar_int(cases[case_index].right);
        BennuValue lifted_result = {0};
        const int64_t values[] = {
            cases[case_index].implementation == BENNU_IMPL_MUL_INT ? INT64_C(1) : INT64_C(0),
            INT64_MIN, INT64_MIN};
        const size_t allocations_before = bennu_probe_outstanding_allocations;
        lifted_resources.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
        if (!bennu_literal_int(&lifted_resources, &lifted_left, values, 3U,
                               "vector-literal", context, context) ||
            bennu_apply(&lifted_resources, cases[case_index].implementation,
                        &lifted_result, &lifted_left, &lifted_right,
                        cases[case_index].arity, cases[case_index].name,
                        cases[case_index].primitive_id, primary, context) ||
            lifted_resources.failure != BENNU_FAILURE_DOMAIN ||
            lifted_resources.failure_implementation != cases[case_index].implementation ||
            lifted_resources.failure_primitive_id != cases[case_index].primitive_id ||
            lifted_resources.failure_domain_reason !=
                BENNU_DOMAIN_INTEGER_OVERFLOW ||
            strcmp(lifted_resources.failure_admission_point, cases[case_index].name) != 0 ||
            lifted_resources.failure_signature.parameter_count != cases[case_index].arity ||
            lifted_resources.failure_signature.parameter_types[0] != BENNU_INT ||
            lifted_resources.failure_signature.result_type != BENNU_INT ||
            lifted_resources.failure_operand_count != cases[case_index].arity ||
            lifted_resources.failure_left_operand.type != BENNU_INT ||
            lifted_resources.failure_left_operand.integer != INT64_MIN ||
            (cases[case_index].arity == 2U &&
             (lifted_resources.failure_signature.parameter_types[1] != BENNU_INT ||
              lifted_resources.failure_right_operand.type != BENNU_INT ||
              lifted_resources.failure_right_operand.integer != cases[case_index].right)) ||
            lifted_resources.failure_has_element_index == 0 ||
            lifted_resources.failure_element_index != 1U ||
            lifted_resources.failure_primary_span.begin.offset != 7U ||
            lifted_resources.failure_primary_span.begin.line != 3U ||
            lifted_resources.failure_primary_span.begin.column != 2U ||
            lifted_resources.failure_primary_span.end.offset != 11U ||
            lifted_resources.failure_primary_span.end.line != 3U ||
            lifted_resources.failure_primary_span.end.column != 6U ||
            lifted_resources.failure_context_span.begin.offset != 5U ||
            lifted_resources.failure_context_span.begin.line != 3U ||
            lifted_resources.failure_context_span.begin.column != 1U ||
            lifted_resources.failure_context_span.end.offset != 40U ||
            lifted_resources.failure_context_span.end.line != 3U ||
            lifted_resources.failure_context_span.end.column != 36U ||
            lifted_resources.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
            lifted_resources.work_units != 3U ||
            lifted_resources.live_bytes != 3U * sizeof(int64_t) ||
            lifted_resources.reservation_ordinal != 2U ||
            lifted_result.data != NULL ||
            bennu_probe_outstanding_allocations != allocations_before + 1U ||
            !bennu_failure_context_valid(&lifted_resources)) {
          bennu_release(&lifted_resources, &lifted_left);
          return 2;
        }
        bennu_release(&lifted_resources, &lifted_left);
        if (lifted_resources.live_bytes != 0U ||
            bennu_probe_outstanding_allocations != allocations_before) {
          return 3;
        }
      }
    }
  }
  return 0;
}
)bennu_c";
  source += R"bennu_c(

static int bennu_probe_checked_resource_matrix(void) {
  static const BennuCheckedIntCase cases[] = {
      {"dec", BENNU_IMPL_DEC_INT, BENNU_PRIMITIVE_DEC, 1U, INT64_C(0)},
      {"neg", BENNU_IMPL_NEG_INT, BENNU_PRIMITIVE_NEG, 1U, INT64_C(0)},
      {"abs", BENNU_IMPL_ABS_INT, BENNU_PRIMITIVE_ABS, 1U, INT64_C(0)},
      {"sub", BENNU_IMPL_SUB_INT, BENNU_PRIMITIVE_SUB, 2U, INT64_C(1)},
      {"mul", BENNU_IMPL_MUL_INT, BENNU_PRIMITIVE_MUL, 2U, INT64_C(2)}};
  const BennuSourceSpan span = bennu_source_span(
      bennu_source_location(1U, 1U, 1U), bennu_source_location(8U, 1U, 8U));
  size_t case_index = 0U;
  for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
    BennuResources allocation = {0};
    BennuValue input = {0};
    BennuValue right = bennu_scalar_int(cases[case_index].right);
    BennuValue result = {0};
    const int64_t values[] = {INT64_C(1), INT64_C(2), INT64_C(3)};
    const size_t allocations_before = bennu_probe_outstanding_allocations;
    allocation.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
    if (!bennu_literal_int(&allocation, &input, values, 3U, "vector-literal",
                           span, span)) {
      return 1;
    }
    allocation.has_failure_ordinal = 1;
    allocation.failure_ordinal = allocation.reservation_ordinal;
    if (bennu_apply(&allocation, cases[case_index].implementation, &result,
                    &input, &right, cases[case_index].arity,
                    cases[case_index].name, cases[case_index].primitive_id,
                    span, span) || allocation.failure != BENNU_FAILURE_ALLOCATION ||
        allocation.failure_primitive_id != cases[case_index].primitive_id ||
        strcmp(allocation.failure_admission_point, cases[case_index].name) != 0 ||
        allocation.failure_requested_elements != 3U ||
        allocation.failure_requested_bytes != 3U * sizeof(int64_t) ||
        allocation.work_units != 0U || allocation.live_bytes != 3U * sizeof(int64_t) ||
        result.data != NULL ||
        bennu_probe_outstanding_allocations != allocations_before + 1U) {
      bennu_release(&allocation, &input);
      return 2;
    }
    bennu_release(&allocation, &input);
    if (allocation.live_bytes != 0U ||
        bennu_probe_outstanding_allocations != allocations_before) {
      return 3;
    }
    {
      BennuResources work = {0};
      BennuValue scalar = bennu_scalar_int(INT64_C(3));
      BennuValue scalar_result = {0};
      work.profile = BENNU_PROFILE_BOUNDED_V1;
      work.has_work_limit = 1;
      work.work_limit = 0U;
      if (bennu_apply(&work, cases[case_index].implementation, &scalar_result,
                      &scalar, &right, cases[case_index].arity,
                      cases[case_index].name, cases[case_index].primitive_id,
                      span, span) || work.failure != BENNU_FAILURE_PROFILE ||
          work.failure_limit != BENNU_LIMIT_MAX_WORK_UNITS ||
          work.failure_configured_limit != 0U || work.failure_usage_before != 0U ||
          work.failure_refused_charge != 1U || work.work_units != 0U ||
          work.live_bytes != 0U || scalar_result.data != NULL) {
        return 4;
      }
    }
    {
      BennuResources empty_resources = {0};
      BennuValue empty = {BENNU_VECTOR, BENNU_INT, 0U, 0U, INT64_C(0), 0.0, NULL};
      BennuValue empty_result = {0};
      empty_resources.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
      if (!bennu_apply(&empty_resources, cases[case_index].implementation,
                       &empty_result, &empty, &right, cases[case_index].arity,
                       cases[case_index].name, cases[case_index].primitive_id,
                       span, span) || empty_result.container != BENNU_VECTOR ||
          empty_result.count != 0U || empty_result.data != NULL ||
          empty_resources.work_units != 0U || empty_resources.live_bytes != 0U) {
        return 5;
      }
      bennu_release(&empty_resources, &empty_result);
    }
  }
  for (case_index = 3U; case_index < 5U; ++case_index) {
    BennuResources shape = {0};
    BennuValue argument = {BENNU_VECTOR, BENNU_INT, 3U, 0U, INT64_C(0), 0.0, NULL};
    shape.profile = BENNU_PROFILE_TRUSTED_LOCAL_V1;
    if (bennu_require_shape(&shape, cases[case_index].name,
                            cases[case_index].primitive_id, 2U, 2U, &argument,
                            span, span) || shape.failure != BENNU_FAILURE_SHAPE ||
        shape.work_units != 0U || shape.live_bytes != 0U) {
      return 6;
    }
  }
  return 0;
}

int main(void) {
  _Static_assert(BENNU_PRIMITIVE_NONE == -1, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_INC == 0, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_ADD == 1, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_EQUALS == 2, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_NOT == 3, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_IOTA == 4, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_DEC == 5, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_NEG == 6, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_ABS == 7, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_SUB == 8, "primitive id mismatch");
  _Static_assert(BENNU_PRIMITIVE_MUL == 9, "primitive id mismatch");
  _Static_assert(BENNU_DOMAIN_INTEGER_OVERFLOW == 1,
                 "domain reason mismatch");
  {
    size_t iteration = 0U;
    for (iteration = 0U; iteration < 2U; ++iteration) {
      BennuResources snapshot = {0};
      if (bennu_execute(&snapshot) == 0 ||
          snapshot.failure != BENNU_FAILURE_DOMAIN ||
          snapshot.profile != BENNU_PROFILE_TRUSTED_LOCAL_V1 ||
          snapshot.failure_implementation != BENNU_IMPL_ADD_INT ||
          snapshot.failure_primitive_id != BENNU_PRIMITIVE_ADD ||
          snapshot.failure_signature.parameter_count != 2U ||
          snapshot.failure_signature.parameter_types[0] != BENNU_INT ||
          snapshot.failure_signature.parameter_types[1] != BENNU_INT ||
          snapshot.failure_signature.result_type != BENNU_INT ||
          snapshot.failure_operand_count != 2U ||
          snapshot.failure_left_operand.type != BENNU_INT ||
          snapshot.failure_left_operand.integer != INT64_MAX ||
          snapshot.failure_right_operand.type != BENNU_INT ||
          snapshot.failure_right_operand.integer != INT64_C(1) ||
          snapshot.failure_has_element_index == 0 ||
          snapshot.failure_element_index != 1U ||
          snapshot.failure_has_requested_elements != 0 ||
          snapshot.failure_has_requested_bytes != 0 ||
          snapshot.failure_primary_span.begin.line != 2U ||
          snapshot.failure_primary_span.begin.column != 1U ||
          snapshot.failure_primary_span.end.column != 4U ||
          snapshot.failure_context_span.begin.line != 2U ||
          snapshot.failure_context_span.begin.column != 1U ||
          snapshot.failure_context_span.end.column != 35U ||
          snapshot.live_bytes != 0U ||
          bennu_probe_outstanding_allocations != 0U ||
          !bennu_failure_context_valid(&snapshot)) {
        return 1;
      }
    }
  }
  const int domain = bennu_probe_domain_context();
  const int profiles = bennu_probe_profile_contexts();
  const int live = bennu_probe_live_context();
  const int shape = bennu_probe_shape_context();
  const int checked_bits = bennu_probe_checked_double_bits();
  const int checked_environment =
      bennu_probe_hostile_checked_double_environment();
  const int checked_contexts = bennu_probe_checked_int_contexts();
  int checked_resources = 0;
  {
    size_t iteration = 0U;
    for (iteration = 0U; iteration < 2U; ++iteration) {
      checked_resources = bennu_probe_checked_resource_matrix();
      if (checked_resources != 0) {
        break;
      }
    }
  }
  if (domain != 0) {
    return 20 + domain;
  }
  if (profiles != 0) {
    return 30 + profiles;
  }
  if (live != 0) {
    return 40 + live;
  }
  if (shape != 0) {
    return 50 + shape;
  }
  if (checked_bits != 0) {
    return 60 + checked_bits;
  }
  if (checked_environment != 0) {
    return 70 + checked_environment;
  }
  if (checked_contexts != 0) {
    return 80 + checked_contexts;
  }
  if (checked_resources != 0) {
    return 90 + checked_resources;
  }
  return bennu_probe_invalid_free == 0 &&
                 bennu_probe_total_allocations == bennu_probe_total_frees &&
                 bennu_probe_outstanding_allocations == 0U
             ? 0
             : 99;
}
)bennu_c";
  return source;
}

std::string refusal_evidence(const bennu::Error &error) {
  if (!error.resource.has_value() || !error.primitive.has_value() ||
      !error.resource->limit_kind.has_value() ||
      !error.resource->configured_limit.has_value() ||
      !error.resource->usage_before.has_value() ||
      !error.resource->refused_charge.has_value()) {
    return {};
  }
  const std::string_view reason =
      error.resource->reason == bennu::ResourceErrorReason::profile_limit
          ? "profile_limit"
          : "unexpected";
  std::string_view limit = "unexpected";
  if (*error.resource->limit_kind ==
      bennu::ResourceLimitKind::max_vector_bytes) {
    limit = "max_vector_bytes";
  } else if (*error.resource->limit_kind ==
             bennu::ResourceLimitKind::max_live_evaluation_bytes) {
    limit = "max_live_evaluation_bytes";
  } else if (*error.resource->limit_kind ==
             bennu::ResourceLimitKind::max_work_units) {
    limit = "max_work_units";
  }
  return "bennu-source:" + std::to_string(error.location.line) + ":" +
         std::to_string(error.location.column) + ": ResourceError: reason=" +
         std::string(reason) +
         " profile=" + error.resource->profile +
         " limit=" + std::string(limit) +
         " configured=" +
         std::to_string(*error.resource->configured_limit) +
         " usage-before=" + std::to_string(*error.resource->usage_before) +
         " refused-charge=" +
         std::to_string(*error.resource->refused_charge) +
         " admission=" + error.primitive->name +
         " source=" + std::to_string(error.location.offset) + ":" +
         std::to_string(error.location.line) + ":" +
         std::to_string(error.location.column) + "\n";
}

} // namespace

int main(int argument_count, char **arguments) {
  if (argument_count != 30) {
    return 2;
  }

  std::string corpus_source;
  std::string corpus_expected;
  if (!read_file(arguments[2], corpus_source) ||
      !read_file(arguments[3], corpus_expected)) {
    return 3;
  }
  normalize_newlines(corpus_expected);
  bennu::ProgramResult corpus = bennu::evaluate_source(corpus_source);
  if (!corpus.ok) {
    destroy_program(corpus);
    return 4;
  }
  std::string corpus_actual;
  for (bennu::Value &value : corpus.values) {
    const bennu::ValueFormattingResult formatted = bennu::format_value(value);
    if (!formatted.ok) {
      destroy_program(corpus);
      return 5;
    }
    corpus_actual += formatted.formatted;
    corpus_actual.push_back('\n');
  }
  destroy_program(corpus);
  if (corpus_actual != corpus_expected) {
    return 6;
  }

  struct ErrorCase {
    std::string_view source;
    bennu::ErrorKind kind;
  };
  const ErrorCase error_cases[] = {
      {"é", bennu::ErrorKind::invalid_byte},
      {"12x", bennu::ErrorKind::malformed_literal},
      {"9223372036854775808", bennu::ErrorKind::literal_range_error},
      {"inc ", bennu::ErrorKind::syntax_error},
      {"wat[1]", bennu::ErrorKind::unknown_name},
      {"add[1]", bennu::ErrorKind::arity_error},
      {"add[1 true]", bennu::ErrorKind::type_mismatch},
      {"add[(1 2) (3)]", bennu::ErrorKind::shape_mismatch},
      {"inc 9223372036854775807", bennu::ErrorKind::domain_error},
      {"iota[2305843009213693952]", bennu::ErrorKind::resource_error},
  };
  for (const ErrorCase &error_case : error_cases) {
    bennu::ValueResult failure = bennu::evaluate_expression(error_case.source);
    if (failure.ok || failure.error.kind != error_case.kind) {
      if (failure.ok) {
        bennu::destroy_value(failure.value);
      }
      return 7;
    }
  }
  bennu::ValueResult domain_context =
      bennu::evaluate_expression("inc[(9223372036854775807)]");
  if (domain_context.ok ||
      domain_context.error.kind != bennu::ErrorKind::domain_error ||
      !domain_context.error.primitive.has_value() ||
      domain_context.error.primitive->id != bennu::PrimitiveId::inc ||
      domain_context.error.primitive->name != "inc" ||
      !domain_context.error.domain.has_value() ||
      domain_context.error.domain->reason !=
          bennu::DomainErrorReason::integer_overflow ||
      domain_context.error.domain->signature.parameter_types.size() != 1U ||
      domain_context.error.domain->signature.parameter_types[0] !=
          bennu::ScalarType::integer ||
      domain_context.error.domain->signature.result_type !=
          bennu::ScalarType::integer ||
      domain_context.error.domain->operands.size() != 1U ||
      domain_context.error.domain->operands[0].type !=
          bennu::ScalarType::integer ||
      domain_context.error.domain->operands[0].integer != INT64_MAX ||
      domain_context.error.element_index != std::size_t{0U} ||
      domain_context.error.location.offset != std::size_t{1U} ||
      domain_context.error.location.line != std::size_t{1U} ||
      domain_context.error.location.column != std::size_t{1U}) {
    if (domain_context.ok) {
      bennu::destroy_value(domain_context.value);
    }
    return 9;
  }
  bennu::ProgramResult late_error =
      bennu::evaluate_source("inc 5\nwat[1]\n");
  if (late_error.ok || !late_error.values.empty() ||
      late_error.error.kind != bennu::ErrorKind::unknown_name ||
      late_error.error.location.line != 2U) {
    destroy_program(late_error);
    return 8;
  }

  constexpr std::string_view profile_source =
      "inc[(1)]\n"
      "inc[(1)]\n";
  const bennu::EvaluationConfiguration exact_profile{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{24U},
                            std::size_t{2U}},
      bennu::AllocationFailureInjection{std::nullopt}};
  for (std::size_t invocation = 0U; invocation < 2U; ++invocation) {
    bennu::ProgramResult exact =
        bennu::evaluate_source(profile_source, exact_profile);
    if (!exact.ok || exact.values.size() != 2U) {
      destroy_program(exact);
      return 20;
    }
    destroy_program(exact);
  }

  const bennu::EvaluationConfiguration one_past_profile{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{24U},
                            std::size_t{1U}},
      bennu::AllocationFailureInjection{std::nullopt}};
  bennu::ProgramResult refused =
      bennu::evaluate_source(profile_source, one_past_profile);
  if (refused.ok || !refused.values.empty() ||
      !is_resource_error(refused.error,
                         bennu::ResourceErrorReason::profile_limit) ||
      refused.error.resource->limit_kind !=
          bennu::ResourceLimitKind::max_work_units ||
      refused.error.resource->configured_limit != std::size_t{1U} ||
      refused.error.resource->usage_before != std::size_t{1U} ||
      refused.error.resource->refused_charge != std::size_t{1U} ||
      refused.error.resource->profile != "bounded-v1" ||
      !refused.error.primitive.has_value() ||
      refused.error.primitive->name != "inc" ||
      refused.error.location.offset != std::size_t{10U} ||
      refused.error.location.line != std::size_t{2U} ||
      refused.error.location.column != std::size_t{1U}) {
    destroy_program(refused);
    return 21;
  }
  const std::string expected_refusal = refusal_evidence(refused.error);
  if (expected_refusal.empty() ||
      !write_file(arguments[14], expected_refusal)) {
    return 22;
  }

  constexpr std::string_view vector_refusal_source = "iota[2]\n";
  const bennu::EvaluationConfiguration vector_refusal_configuration{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{64U},
                            std::size_t{64U}},
      bennu::AllocationFailureInjection{std::nullopt}};
  bennu::ProgramResult vector_refused = bennu::evaluate_source(
      vector_refusal_source, vector_refusal_configuration);
  if (vector_refused.ok || !vector_refused.values.empty() ||
      !is_resource_error(vector_refused.error,
                         bennu::ResourceErrorReason::profile_limit) ||
      vector_refused.error.resource->limit_kind !=
          bennu::ResourceLimitKind::max_vector_bytes ||
      vector_refused.error.resource->configured_limit != std::size_t{8U} ||
      vector_refused.error.resource->usage_before != std::size_t{0U} ||
      vector_refused.error.resource->refused_charge != std::size_t{16U} ||
      vector_refused.error.resource->requested_elements != std::size_t{2U} ||
      vector_refused.error.resource->requested_bytes != std::size_t{16U} ||
      !vector_refused.error.primitive.has_value() ||
      vector_refused.error.primitive->name != "iota") {
    destroy_program(vector_refused);
    return 29;
  }
  const std::string vector_refusal_expected =
      refusal_evidence(vector_refused.error);
  if (vector_refusal_expected.empty() ||
      !write_file(arguments[18], vector_refusal_expected)) {
    return 30;
  }

  constexpr std::string_view live_refusal_source = "inc[(1)]\n";
  const bennu::EvaluationConfiguration live_refusal_configuration{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::size_t{8U}, std::size_t{8U},
                            std::size_t{2U}},
      bennu::AllocationFailureInjection{std::nullopt}};
  bennu::ProgramResult live_refused =
      bennu::evaluate_source(live_refusal_source, live_refusal_configuration);
  if (live_refused.ok || !live_refused.values.empty() ||
      !is_resource_error(live_refused.error,
                         bennu::ResourceErrorReason::profile_limit) ||
      live_refused.error.resource->limit_kind !=
          bennu::ResourceLimitKind::max_live_evaluation_bytes ||
      live_refused.error.resource->configured_limit != std::size_t{8U} ||
      live_refused.error.resource->usage_before != std::size_t{8U} ||
      live_refused.error.resource->refused_charge != std::size_t{8U} ||
      live_refused.error.resource->requested_elements != std::size_t{1U} ||
      live_refused.error.resource->requested_bytes != std::size_t{8U} ||
      !live_refused.error.primitive.has_value() ||
      live_refused.error.primitive->name != "inc") {
    destroy_program(live_refused);
    return 31;
  }
  const std::string live_refusal_expected = refusal_evidence(live_refused.error);
  if (live_refusal_expected.empty() ||
      !write_file(arguments[21], live_refusal_expected)) {
    return 32;
  }

  const bennu::EvaluationConfiguration fail_first{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::size_t{0U}}};
  for (std::size_t invocation = 0U; invocation < 2U; ++invocation) {
    bennu::ValueResult iota =
        bennu::evaluate_expression("iota[2]", fail_first);
    if (iota.ok ||
        !is_resource_error(iota.error,
                           bennu::ResourceErrorReason::allocation_unavailable) ||
        iota.error.resource->requested_elements != std::size_t{2U} ||
        iota.error.resource->requested_bytes != std::size_t{16U}) {
      if (iota.ok) {
        bennu::destroy_value(iota.value);
      }
      return 23;
    }
  }

  const bennu::EvaluationConfiguration fail_second{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::size_t{1U}}};
  const bennu::EvaluationConfiguration fail_third{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::size_t{2U}}};
  bennu::ValueResult lifted =
      bennu::evaluate_expression("inc[(1 2)]", fail_second);
  if (lifted.ok ||
      !is_resource_error(lifted.error,
                         bennu::ResourceErrorReason::allocation_unavailable) ||
      lifted.error.resource->requested_elements != std::size_t{2U} ||
      lifted.error.resource->requested_bytes != std::size_t{16U} ||
      !lifted.error.primitive.has_value() ||
      lifted.error.primitive->name != "inc" ||
      lifted.error.location.offset != std::size_t{1U} ||
      lifted.error.location.line != std::size_t{1U} ||
      lifted.error.location.column != std::size_t{1U}) {
    if (lifted.ok) {
      bennu::destroy_value(lifted.value);
    }
    return 24;
  }
  bennu::ProgramResult late =
      bennu::evaluate_source("iota[2]\niota[2]\n", fail_second);
  if (late.ok || !late.values.empty() ||
      !is_resource_error(late.error,
                         bennu::ResourceErrorReason::allocation_unavailable) ||
      late.error.resource->requested_elements != std::size_t{2U} ||
      late.error.resource->requested_bytes != std::size_t{16U} ||
      !late.error.primitive.has_value() ||
      late.error.primitive->name != "iota" ||
      late.error.location.offset != std::size_t{9U} ||
      late.error.location.line != std::size_t{2U} ||
      late.error.location.column != std::size_t{1U}) {
    destroy_program(late);
    return 25;
  }

  constexpr std::string_view shape_before_resource_source =
      "add[iota[3] (9223372036854775807 9223372036854775807)]\n";
  bennu::ValueResult shape_before_resource = bennu::evaluate_expression(
      shape_before_resource_source, fail_third);
  if (shape_before_resource.ok ||
      shape_before_resource.error.kind != bennu::ErrorKind::shape_mismatch ||
      shape_before_resource.error.argument_position != std::size_t{1U} ||
      !shape_before_resource.error.shape.has_value() ||
      shape_before_resource.error.shape->expected !=
          std::vector<std::size_t>{2U} ||
      shape_before_resource.error.shape->actual !=
          std::vector<std::size_t>{3U} ||
      !shape_before_resource.error.primitive.has_value() ||
      shape_before_resource.error.primitive->name != "add" ||
      shape_before_resource.error.location.line != std::size_t{1U} ||
      shape_before_resource.error.location.column != std::size_t{5U}) {
    if (shape_before_resource.ok) {
      bennu::destroy_value(shape_before_resource.value);
    }
    return 34;
  }

  constexpr std::string_view resource_before_domain_source =
      "add[(9223372036854775807) (1)]\n";
  bennu::ValueResult resource_before_domain = bennu::evaluate_expression(
      resource_before_domain_source, fail_third);
  if (resource_before_domain.ok ||
      !is_resource_error(
          resource_before_domain.error,
          bennu::ResourceErrorReason::allocation_unavailable) ||
      resource_before_domain.error.resource->requested_elements !=
          std::size_t{1U} ||
      resource_before_domain.error.resource->requested_bytes !=
          std::size_t{8U} ||
      resource_before_domain.error.domain.has_value() ||
      !resource_before_domain.error.primitive.has_value() ||
      resource_before_domain.error.primitive->name != "add" ||
      resource_before_domain.error.location.line != std::size_t{1U} ||
      resource_before_domain.error.location.column != std::size_t{1U}) {
    if (resource_before_domain.ok) {
      bennu::destroy_value(resource_before_domain.value);
    }
    return 35;
  }

  struct CheckedResourceCase {
    const char *name;
    bennu::PrimitiveId id;
    const char *vector_source;
    const char *scalar_source;
  };
  const CheckedResourceCase checked_resource_cases[] = {
      {"dec", bennu::PrimitiveId::dec, "dec[(1 2)]", "dec[3]"},
      {"neg", bennu::PrimitiveId::neg, "neg[(1 2)]", "neg[3]"},
      {"abs", bennu::PrimitiveId::abs, "abs[(1 2)]", "abs[3]"},
      {"sub", bennu::PrimitiveId::sub, "sub[(3 4) 1]", "sub[3 1]"},
      {"mul", bennu::PrimitiveId::mul, "mul[(3 4) 2]", "mul[3 2]"},
  };
  const bennu::EvaluationConfiguration refuse_checked_result_allocation{
      bennu::ExecutionProfile::trusted_local_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::nullopt},
      bennu::AllocationFailureInjection{std::size_t{1U}}};
  const bennu::EvaluationConfiguration refuse_checked_scalar_work{
      bennu::ExecutionProfile::bounded_v1,
      bennu::ResourceLimits{std::nullopt, std::nullopt, std::size_t{0U}},
      bennu::AllocationFailureInjection{std::nullopt}};
  for (const CheckedResourceCase &checked_case : checked_resource_cases) {
    bennu::ValueResult allocation = bennu::evaluate_expression(
        checked_case.vector_source, refuse_checked_result_allocation);
    if (allocation.ok ||
        !is_resource_error(
            allocation.error,
            bennu::ResourceErrorReason::allocation_unavailable) ||
        allocation.error.resource->requested_elements != std::size_t{2U} ||
        allocation.error.resource->requested_bytes != std::size_t{16U} ||
        !allocation.error.primitive.has_value() ||
        !allocation.error.primitive->id.has_value() ||
        *allocation.error.primitive->id != checked_case.id ||
        allocation.error.primitive->name != checked_case.name ||
        allocation.error.location.offset != std::size_t{1U} ||
        allocation.error.location.line != std::size_t{1U} ||
        allocation.error.location.column != std::size_t{1U} ||
        allocation.value.vector.booleans.get() != nullptr ||
        allocation.value.vector.integers.get() != nullptr ||
        allocation.value.vector.doubles.get() != nullptr) {
      if (allocation.ok) {
        bennu::destroy_value(allocation.value);
      }
      return 36;
    }

    bennu::ValueResult work = bennu::evaluate_expression(
        checked_case.scalar_source, refuse_checked_scalar_work);
    if (work.ok ||
        !is_resource_error(work.error,
                           bennu::ResourceErrorReason::profile_limit) ||
        work.error.resource->limit_kind !=
            bennu::ResourceLimitKind::max_work_units ||
        work.error.resource->configured_limit != std::size_t{0U} ||
        work.error.resource->usage_before != std::size_t{0U} ||
        work.error.resource->refused_charge != std::size_t{1U} ||
        work.error.resource->requested_elements.has_value() ||
        work.error.resource->requested_bytes.has_value() ||
        !work.error.primitive.has_value() ||
        !work.error.primitive->id.has_value() ||
        *work.error.primitive->id != checked_case.id ||
        work.error.primitive->name != checked_case.name ||
        work.error.location.offset != std::size_t{1U} ||
        work.error.location.line != std::size_t{1U} ||
        work.error.location.column != std::size_t{1U}) {
      if (work.ok) {
        bennu::destroy_value(work.value);
      }
      return 37;
    }
  }

  bennu::ValueResult overflow = bennu::evaluate_expression(
      "iota[2305843009213693952]", exact_profile);
  if (overflow.ok ||
      !is_resource_error(overflow.error,
                         bennu::ResourceErrorReason::size_overflow) ||
      overflow.error.resource->requested_elements !=
          std::size_t{2305843009213693952ULL} ||
      overflow.error.resource->requested_bytes.has_value() ||
      overflow.error.resource->profile != "bounded-v1" ||
      !overflow.error.primitive.has_value() ||
      overflow.error.primitive->name != "iota" ||
      overflow.error.location.offset != std::size_t{1U} ||
      overflow.error.location.line != std::size_t{1U} ||
      overflow.error.location.column != std::size_t{1U}) {
    if (overflow.ok) {
      bennu::destroy_value(overflow.value);
    }
    return 26;
  }

  if (!emit_and_build(profile_source, exact_profile, arguments[4], arguments[5],
                      arguments[1]) ||
      !emit_probe_and_build(profile_source, one_past_profile, arguments[12],
                            arguments[13], arguments[1],
                            work_refusal_assertions) ||
      !emit_probe_and_build("iota[2]\n", fail_first, arguments[6], arguments[7],
                            arguments[1], allocation_iota_assertions) ||
      !emit_probe_and_build("inc[(1 2)]\n", fail_second, arguments[8],
                            arguments[9], arguments[1],
                            allocation_lifted_assertions) ||
      !emit_probe_and_build("iota[2]\niota[2]\n", fail_second, arguments[10],
                            arguments[11], arguments[1],
                            allocation_late_assertions) ||
      !emit_probe_and_build(vector_refusal_source,
                            vector_refusal_configuration, arguments[16],
                            arguments[17], arguments[1],
                            vector_refusal_assertions) ||
      !emit_probe_and_build(live_refusal_source, live_refusal_configuration,
                            arguments[19], arguments[20], arguments[1],
                            live_refusal_assertions) ||
      !emit_probe_and_build("iota[2305843009213693952]\n", fail_first,
                            arguments[23], arguments[24], arguments[1],
                            size_assertions) ||
      !emit_probe_and_build(shape_before_resource_source, fail_third,
                            arguments[25], arguments[26], arguments[1],
                            shape_before_resource_assertions) ||
      !emit_probe_and_build(resource_before_domain_source, fail_third,
                            arguments[27], arguments[28], arguments[1],
                            resource_before_domain_assertions)) {
    return 27;
  }
  const std::optional<std::string> context_probe = generated_runtime_probe();
  if (!context_probe.has_value() ||
      !write_file(arguments[15], *context_probe)) {
    return 28;
  }
  const bennu::NativeBuildResult context_probe_native = bennu::build_native(
      bennu::NativeBuildRequest{*context_probe, arguments[22], arguments[1], ""});
  if (!context_probe_native.ok) {
    return 33;
  }
  const std::optional<std::string> standard_fenv_probe =
      generated_standard_fenv_probe();
  if (!standard_fenv_probe.has_value()) {
    return 38;
  }
  const bennu::NativeBuildResult standard_fenv_probe_native =
      bennu::build_native(bennu::NativeBuildRequest{
          *standard_fenv_probe, arguments[29], arguments[1], ""});
  if (!standard_fenv_probe_native.ok) {
    return 39;
  }
  return 0;
}
