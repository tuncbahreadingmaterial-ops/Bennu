#include "cli_output.hpp"

#include <iostream>
#include <streambuf>
#include <string_view>

namespace {

class FailingWriteBuffer : public std::streambuf {
protected:
  std::streamsize xsputn(const char *, std::streamsize) override { return 0; }

  int_type overflow(int_type) override { return traits_type::eof(); }
};

class FailingFlushBuffer : public std::streambuf {
protected:
  int sync() override { return -1; }
};

class FailAfterOneWriteBuffer : public std::streambuf {
protected:
  std::streamsize xsputn(const char *, std::streamsize count) override {
    if (!wrote_once_) {
      wrote_once_ = true;
      return count;
    }
    return count == 0 ? 0 : count - 1;
  }

private:
  bool wrote_once_ = false;
};

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
  }
  return true;
}

} // namespace

int main() {
  bool ok = true;
  {
    FailingWriteBuffer buffer;
    std::ostream output(&buffer);
    ok &= expect(!bennu_cli::write_stdout(output, "result\n"),
                 "write failure must be observable");
  }
  {
    FailingFlushBuffer buffer;
    std::ostream output(&buffer);
    ok &= expect(!bennu_cli::flush_stdout(output),
                 "flush failure must be observable");
  }
  {
    FailAfterOneWriteBuffer buffer;
    std::ostream output(&buffer);
    ok &= expect(bennu_cli::write_stdout(output, "first result\n"),
                 "complete first write must succeed");
    ok &= expect(!bennu_cli::write_stdout(output, "second result\n"),
                 "partial later write must be observable");
  }
  return ok ? 0 : 1;
}
