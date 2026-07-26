# TEST-ID: DECISION-RECORD-POLICY
if(NOT DEFINED BENNU_SOURCE_DIR)
  message(FATAL_ERROR "BENNU_SOURCE_DIR is required")
endif()

file(READ "${BENNU_SOURCE_DIR}/doc/decision-diary.md" compatibility_pointer)
string(REPLACE "\r\n" "\n" compatibility_pointer "${compatibility_pointer}")
set(expected_compatibility_pointer [=[# Decision records

New material decisions belong in the issue-owned append-only records described by [`doc/decisions/README.md`](decisions/README.md). Do not append decisions to this compatibility file.

The complete historical diary through Issue #42 is preserved unchanged at [`doc/decisions/legacy-decision-diary.md`](decisions/legacy-decision-diary.md).
]=])
if(NOT compatibility_pointer STREQUAL expected_compatibility_pointer)
  message(FATAL_ERROR
    "doc/decision-diary.md differs from the accepted Issue #65 compatibility pointer")
endif()

file(GLOB issue53_records
  "${BENNU_SOURCE_DIR}/doc/decisions/issue-53-*.md")
list(LENGTH issue53_records issue53_record_count)
if(NOT issue53_record_count EQUAL 1)
  message(FATAL_ERROR
    "Issue #53 must own exactly one decision record; found ${issue53_record_count}")
endif()

set(expected_issue53_record
  "${BENNU_SOURCE_DIR}/doc/decisions/issue-53-sequential-fanout-implementation.md")
list(GET issue53_records 0 issue53_record)
if(NOT issue53_record STREQUAL expected_issue53_record)
  message(FATAL_ERROR
    "Issue #53 decision is misowned or uses an unstable path: ${issue53_record}")
endif()

file(READ "${issue53_record}" issue53_decision)
foreach(required_issue53_text IN ITEMS
    "# Issue #53 — Implement sequential fan-out"
    "https://github.com/tuncbahreadingmaterial-ops/Bennu/issues/53"
    "authoritative owner of the implementation decision"
    "historically appended under"
    "flat fan-out and branch side arenas"
    "immutable views of the still-owned operand"
    "immediately after operand completion and before branch 0"
    "release the initialized result prefix in reverse order"
    "Generated C uses the same pre-branch table point"
    "adds no work charge"
    "Bennu deliberately differs from Anka"
    "tests/fanout_contract.cmake"
    "for authoritative ownership and discovery only")
  string(FIND "${issue53_decision}" "${required_issue53_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR
      "Issue #53 authoritative decision is incomplete: ${required_issue53_text}")
  endif()
endforeach()

file(READ "${BENNU_SOURCE_DIR}/README.md" readme)
foreach(required_readme_text IN ITEMS
    "`add [1 2]`"
    "statically spreads a tuple operand"
    "exactly one level"
    "adjacent direct call remains one argument"
    "not recursive"
    "no explicit mixed-argument"
    "spread syntax")
  string(FIND "${readme}" "${required_readme_text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR
      "README tuple spreading contract is missing: ${required_readme_text}")
  endif()
endforeach()

string(FIND "${readme}" "tuple spreading is not yet part of Bennu"
  stale_tuple_claim_at)
if(NOT stale_tuple_claim_at EQUAL -1)
  message(FATAL_ERROR "README still claims that tuple spreading is unavailable")
endif()
