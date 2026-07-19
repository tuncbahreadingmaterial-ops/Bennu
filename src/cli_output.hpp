#pragma once

#include <cstddef>
#include <iosfwd>
#include <string_view>

namespace bennu_cli {

struct OutputWriteResult {
  bool ok;
  std::size_t byte_count;
};

using OutputWriteFunction = OutputWriteResult (*)(void *, std::string_view);
using OutputFlushFunction = bool (*)(void *);

struct OutputSink {
  void *context;
  OutputWriteFunction write;
  OutputFlushFunction flush;
};

bool write_stdout(OutputSink output, std::string_view text);
bool flush_stdout(OutputSink output);
bool write_stdout(std::ostream &output, std::string_view text);
bool flush_stdout(std::ostream &output);

} // namespace bennu_cli
