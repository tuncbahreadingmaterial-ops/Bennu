#include "cli_output.hpp"

#include <ostream>

namespace bennu_cli {

bool write_stdout(std::ostream &output, std::string_view text) {
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return output.good();
}

bool flush_stdout(std::ostream &output) {
  output.flush();
  return output.good();
}

} // namespace bennu_cli
