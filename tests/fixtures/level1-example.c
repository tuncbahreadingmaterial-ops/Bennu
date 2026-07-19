/* Generated deterministically by Bennu. Standard C11. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static int bennu_print_integer(int64_t value) {
  return fprintf(stdout, ">>%" PRId64 "\n", value) < 0;
}

static int bennu_print_array(int64_t count) {
  if (fputs(">>(", stdout) == EOF) {
    return 1;
  }
  for (int64_t value = INT64_C(1); value <= count; ++value) {
    if (value != INT64_C(1) && fputc(' ', stdout) == EOF) {
      return 1;
    }
    if (fprintf(stdout, "%" PRId64, value) < 0) {
      return 1;
    }
  }
  return fputs(")\n", stdout) == EOF;
}

int main(void) {
  if (bennu_print_array(INT64_C(5)) != 0) {
    return 1;
  }
  if (bennu_print_integer(INT64_C(6)) != 0) {
    return 1;
  }
  return fflush(stdout) == 0 ? 0 : 1;
}
