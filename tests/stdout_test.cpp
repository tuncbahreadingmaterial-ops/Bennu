#include "cli_output.hpp"

#include <cstddef>
#include <iostream>
#include <streambuf>
#include <string_view>

namespace {

struct TestOutput {
  std::size_t complete_writes_remaining;
  bool flush_succeeds;
};

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
  }
  return true;
}

bool flush_test_output(void *context) {
  const auto &output = *static_cast<TestOutput *>(context);
  return output.flush_succeeds;
}

bennu_cli::OutputWriteResult write_test_output(void *context,
                                                std::string_view text) {
  auto &output = *static_cast<TestOutput *>(context);
  if (output.complete_writes_remaining != 0) {
    --output.complete_writes_remaining;
    return bennu_cli::OutputWriteResult{true, text.size()};
  }

  const std::size_t partial_count = text.empty() ? 0 : text.size() - 1;
  return bennu_cli::OutputWriteResult{false, partial_count};
}

bennu_cli::OutputSink make_test_output(TestOutput &output) {
  return bennu_cli::OutputSink{&output, write_test_output, flush_test_output};
}

struct ShortStreamBuffer : std::streambuf {
  std::streamsize xsputn(const char *, std::streamsize count) override {
    return count > 0 ? count - 1 : 0;
  }
};

} // namespace

int main() {
  bool ok = true;
  {
    TestOutput output{0, true};
    ok &= expect(!bennu_cli::write_stdout(make_test_output(output), "result\n"),
                 "write failure must be observable");
  }
  {
    TestOutput output{0, true};
    const bennu_cli::OutputWriteResult result =
        bennu_cli::write_stdout_result(make_test_output(output), "result\n");
    ok &= expect(!result.ok, "detailed write failure must be observable");
    ok &= expect(result.byte_count == 6,
                 "detailed write failure must retain the accepted byte count");
  }
  {
    TestOutput output{0, true};
    const bennu_cli::OutputPublicationResult result =
        bennu_cli::publish_stdout(make_test_output(output), "result\n");
    ok &= expect(!result.ok &&
                     result.reason == bennu_cli::OutputErrorReason::write_failed,
                 "publication must classify a short write");
    ok &= expect(result.pending_byte_count == 7 &&
                     result.accepted_byte_count == 6 &&
                     result.output_position == 6,
                 "short-write context must retain exact byte positions");
    ok &= expect(
        bennu_cli::output_error_record(result) ==
            "bennu_output_error reason=write_failed pending_byte_count=7 accepted_byte_count=6 output_position=6\n",
        "short-write serialization must be stable");
  }
  {
    ShortStreamBuffer buffer{};
    std::ostream output{&buffer};
    const bennu_cli::OutputPublicationResult result =
        bennu_cli::publish_stdout(output, "result\n");
    ok &= expect(!result.ok && result.accepted_byte_count == 6,
                 "ostream publication must retain the exact short-write count");
  }
  {
    TestOutput output{1, false};
    const bennu_cli::OutputPublicationResult result =
        bennu_cli::publish_stdout(make_test_output(output), "result\n");
    ok &= expect(!result.ok &&
                     result.reason == bennu_cli::OutputErrorReason::flush_failed,
                 "publication must classify a final flush failure");
    ok &= expect(
        bennu_cli::output_error_record(result) ==
            "bennu_output_error reason=flush_failed pending_byte_count=7 accepted_byte_count=7 output_position=7\n",
        "flush-failure serialization must be stable");
  }
  {
    TestOutput output{1, false};
    const bennu_cli::OutputSink sink = make_test_output(output);
    ok &= expect(bennu_cli::write_stdout(sink, "result\n"),
                 "write before final flush must succeed");
    ok &= expect(!bennu_cli::flush_stdout(sink),
                 "flush failure must be observable");
  }
  {
    TestOutput output{1, true};
    const bennu_cli::OutputSink sink = make_test_output(output);
    ok &= expect(bennu_cli::write_stdout(sink, "first result\n"),
                 "complete first write must succeed");
    ok &= expect(!bennu_cli::write_stdout(sink, "second result\n"),
                 "partial later write must be observable");
  }
  return ok ? 0 : 1;
}
