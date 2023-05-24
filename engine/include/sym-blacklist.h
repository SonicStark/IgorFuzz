#ifndef _SYM_BLACKLIST_H
#define _SYM_BLACKLIST_H

/* 
  We hope to get ride of ASan stuffs when analysing 
  call stack data generated within __asan_on_error.
  We check the call stack from top to bottom, 
  drop a stack frame if met any of the two rules:
  1. Module name is in blacklist.
  2. Function name is in blacklist.

  If ASan is already enabled, only LSan (a part of ASan)
  and UBSan can coexist with it
  (see https://lists.llvm.org/pipermail/llvm-dev/2016-March/097144.html).

  We generate the following two blacklists
  according to both GNU GCC and LLVM toolchains.
*/

static const char *sym_blacklist_module[] = {

    "libasan",
    "liblsan",
    "libubsan",
    "libclang_rt.",
    NULL

};

static const char *sym_blacklist_function[] = {

    "__asan",
    "__lsan",
    "__sanitizer",
    "__interceptor",
    "__interception",
    "__ubsan",
    "__sancov",
    "__hwasan",
    "__dfsan",
    "__dfsw",
    NULL

};

#endif // _SYM_BLACKLIST_H