#ifndef BENNU_PREADMITTED_TUPLE_HPP
#define BENNU_PREADMITTED_TUPLE_HPP

#include "bennu/resources.hpp"

namespace bennu {

struct PreadmittedTupleAssembly {
  Value value;
  HostArray<Value> fixed_slots{};
  std::size_t element_count{0U};
  std::size_t transferred_count{0U};
  std::size_t transferred_node_count{0U};
  std::size_t transferred_edge_count{0U};
  std::size_t transferred_vector_payload_count{0U};
  std::size_t transferred_reservation_count{0U};
};

struct TupleAssemblyAdmissionResult {
  bool ok;
  PreadmittedTupleAssembly assembly;
  Error error;
};

struct TupleAssemblyOperationResult {
  bool ok;
  Value value;
  ValueInvariant invariant;
};

TupleAssemblyAdmissionResult preadmit_tuple_assembly(
    EvaluationResources &resources, std::size_t element_count,
    std::size_t node_count, std::size_t edge_count,
    std::size_t vector_payload_count, std::size_t nested_reservation_count,
    SourceLocation location, std::string_view producer_name);
TupleAssemblyOperationResult transfer_preadmitted_tuple_element(
    PreadmittedTupleAssembly &assembly, Value &element);
TupleAssemblyOperationResult publish_preadmitted_tuple(
    PreadmittedTupleAssembly &assembly);
ValueReleaseResult release_preadmitted_tuple_assembly(
    EvaluationResources &resources, PreadmittedTupleAssembly &assembly);

} // namespace bennu

#endif
