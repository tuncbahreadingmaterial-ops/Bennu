#pragma once

#include <iosfwd>
#include <string_view>

namespace bennu_cli {

bool write_stdout(std::ostream &output, std::string_view text);
bool flush_stdout(std::ostream &output);

} // namespace bennu_cli
