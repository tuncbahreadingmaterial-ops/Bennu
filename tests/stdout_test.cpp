#include "cli_output.hpp"

#include <cstddef>
#include <iostream>
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

} // namespace

int main() {
  bool ok = true;
  {
    TestOutput output{0, true};
    ok &= expect(!bennu_cli::write_stdout(make_test_output(output), "result\n"),
                 "write failure must be observable");
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
