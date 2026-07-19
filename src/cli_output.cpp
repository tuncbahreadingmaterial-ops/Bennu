#include "cli_output.hpp"

#include <ostream>

namespace {

bennu_cli::OutputWriteResult write_stream(void *context,
                                          std::string_view text) {
  auto &output = *static_cast<std::ostream *>(context);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return bennu_cli::OutputWriteResult{output.good(),
                                      output.good() ? text.size() : 0};
}

bool flush_stream(void *context) {
  auto &output = *static_cast<std::ostream *>(context);
  output.flush();
  return output.good();
}

} // namespace

namespace bennu_cli {

bool write_stdout(OutputSink output, std::string_view text) {
  const OutputWriteResult result = output.write(output.context, text);
  return result.ok && result.byte_count == text.size();
}

bool flush_stdout(OutputSink output) { return output.flush(output.context); }

bool write_stdout(std::ostream &output, std::string_view text) {
  return write_stdout(OutputSink{&output, write_stream, flush_stream}, text);
}

bool flush_stdout(std::ostream &output) {
  return flush_stdout(OutputSink{&output, write_stream, flush_stream});
}

} // namespace bennu_cli
