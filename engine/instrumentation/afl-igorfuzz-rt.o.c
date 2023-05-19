#include "config.h"
#include "types.h"

#include <sanitizer/common_interface_defs.h>

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

/**
 * https://github.com/llvm/llvm-project/blob/main/compiler-rt/include/sanitizer/asan_interface.h
 * void __asan_on_error(void);
 * 
 * https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/asan/asan_report.cpp
 * SANITIZER_INTERFACE_WEAK_DEF(void, __asan_on_error, void) {}
*/
void __asan_on_error(void)
{
  char *__afl_igorfuzz_fpath = getenv(IGORFUZZ_CALLSTACK_ENV_FILEPATH);
  if (!__afl_igorfuzz_fpath) { return; }

  int __afl_igorfuzz_fd = open(
    __afl_igorfuzz_fpath,
    O_WRONLY | O_CREAT | O_TRUNC, 
    IGORFUZZ_CALLSTACK_DEFAULT_MODE
  );
  if (__afl_igorfuzz_fd < 0) { return; }

  __sanitizer_set_report_fd((void *)__afl_igorfuzz_fd);
  __sanitizer_print_stack_trace();
  __sanitizer_set_report_fd((void *)STDERR_FILENO);
  close(__afl_igorfuzz_fd);
}