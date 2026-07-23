#include "cli_output.hpp"

#include <array>
#include <charconv>
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
  const OutputWriteResult result = write_stdout_result(output, text);
  return result.ok && result.byte_count == text.size();
}

OutputWriteResult write_stdout_result(OutputSink output,
                                      std::string_view text) {
  const OutputWriteResult result = output.write(output.context, text);
  return result;
}

bool flush_stdout(OutputSink output) { return output.flush(output.context); }

OutputPublicationResult publish_stdout(OutputSink output,
                                       std::string_view text) {
  const OutputWriteResult written = write_stdout_result(output, text);
  if (!written.ok || written.byte_count != text.size()) {
    return OutputPublicationResult{false, OutputErrorReason::write_failed,
                                   text.size(), written.byte_count,
                                   written.byte_count};
  }
  if (!flush_stdout(output)) {
    return OutputPublicationResult{false, OutputErrorReason::flush_failed,
                                   text.size(), text.size(), text.size()};
  }
  return OutputPublicationResult{true, OutputErrorReason::none, text.size(),
                                 text.size(), text.size()};
}

bool write_stdout(std::ostream &output, std::string_view text) {
  return write_stdout(OutputSink{&output, write_stream, flush_stream}, text);
}

OutputWriteResult write_stdout_result(std::ostream &output,
                                      std::string_view text) {
  return write_stdout_result(OutputSink{&output, write_stream, flush_stream},
                             text);
}

bool flush_stdout(std::ostream &output) {
  return flush_stdout(OutputSink{&output, write_stream, flush_stream});
}

OutputPublicationResult publish_stdout(std::ostream &output,
                                       std::string_view text) {
  return publish_stdout(OutputSink{&output, write_stream, flush_stream}, text);
}

std::string output_error_record(const OutputPublicationResult &result) {
  const auto append_unsigned = [](std::string &output, std::size_t value) {
    std::array<char, 32> digits{};
    const std::to_chars_result converted =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    output.append(digits.data(), converted.ptr);
  };
  std::string record = "bennu_output_error reason=";
  record += result.reason == OutputErrorReason::write_failed ? "write_failed"
                                                             : "flush_failed";
  record += " pending_byte_count=";
  append_unsigned(record, result.pending_byte_count);
  record += " accepted_byte_count=";
  append_unsigned(record, result.accepted_byte_count);
  record += " output_position=";
  append_unsigned(record, result.output_position);
  record.push_back('\n');
  return record;
}

} // namespace bennu_cli
