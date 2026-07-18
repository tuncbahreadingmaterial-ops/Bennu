#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
  if (argc == 1) {
    std::cerr << "error: expected a subcommand or --help\n";
    return 1;
  }

  const std::string_view argument = argv[1];

  if (argc == 2 && argument == "--help") {
    std::cout << "Usage: bennu <command> [arguments]\n"
                 "       bennu --help\n"
                 "\n"
                 "Commands:\n"
                 "  repl    Start an interactive Bennu session\n"
                 "  run     Run a Bennu source file\n"
                 "  emit-c  Emit C source for a Bennu source file\n"
                 "  build   Build a Bennu source file\n";
    return 0;
  }

  if (argument.starts_with('-')) {
    std::cerr << "error: unknown option '" << argument << "'\n";
    return 1;
  }

  if (argument == "repl" || argument == "run" || argument == "emit-c" ||
      argument == "build") {
    std::cerr << "error: subcommand '" << argument << "' is not implemented\n";
    return 1;
  }

  std::cerr << "error: unknown subcommand '" << argument << "'\n";
  return 1;
}
