#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
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

enum class OutputErrorReason {
  none,
  write_failed,
  flush_failed,
};

struct OutputPublicationResult {
  bool ok;
  OutputErrorReason reason;
  std::size_t pending_byte_count;
  std::size_t accepted_byte_count;
  std::size_t output_position;
};

bool write_stdout(OutputSink output, std::string_view text);
OutputWriteResult write_stdout_result(OutputSink output, std::string_view text);
bool flush_stdout(OutputSink output);
OutputPublicationResult publish_stdout(OutputSink output,
                                       std::string_view text);
bool write_stdout(std::ostream &output, std::string_view text);
OutputWriteResult write_stdout_result(std::ostream &output,
                                      std::string_view text);
bool flush_stdout(std::ostream &output);
OutputPublicationResult publish_stdout(std::ostream &output,
                                       std::string_view text);
std::string output_error_record(const OutputPublicationResult &result);

} // namespace bennu_cli
