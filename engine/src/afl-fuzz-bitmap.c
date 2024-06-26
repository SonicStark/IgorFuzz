/*
   american fuzzy lop++ - bitmap related routines
   ----------------------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2023 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     https://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"
#include <limits.h>
#if !defined NAME_MAX
  #define NAME_MAX _XOPEN_NAME_MAX
#endif

#if IGORFUZZ_FEATURE_ENABLE
  #include "sanitizer_symbolizer_tool.h"
#endif

/* Write bitmap to file. The bitmap is useful mostly for the secret
   -B option, to focus a separate fuzzing session on a particular
   interesting input without rediscovering all the others. */

void write_bitmap(afl_state_t *afl) {

  u8  fname[PATH_MAX];
  s32 fd;

  if (!afl->bitmap_changed) { return; }
  afl->bitmap_changed = 0;

  snprintf(fname, PATH_MAX, "%s/fuzz_bitmap", afl->out_dir);
  fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, DEFAULT_PERMISSION);

  if (fd < 0) { PFATAL("Unable to open '%s'", fname); }

  ck_write(fd, afl->virgin_bits, afl->fsrv.map_size, fname);

  close(fd);

}

/* Count the number of bits set in the provided bitmap. Used for the status
   screen several times every second, does not have to be fast. */

u32 count_bits(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = ((afl->fsrv.real_map_size + 3) >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    /* This gets called on the inverse, virgin bitmap; optimize for sparse
       data. */

    if (likely(v == 0xffffffff)) {

      ret += 32;
      continue;

    }

    v -= ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    ret += (((v + (v >> 4)) & 0xF0F0F0F) * 0x01010101) >> 24;

  }

  return ret;

}

/* Count the number of bytes set in the bitmap. Called fairly sporadically,
   mostly to update the status screen or calibrate and examine confirmed
   new paths. */

u32 count_bytes(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = ((afl->fsrv.real_map_size + 3) >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    if (likely(!v)) { continue; }
    if (v & 0x000000ffU) { ++ret; }
    if (v & 0x0000ff00U) { ++ret; }
    if (v & 0x00ff0000U) { ++ret; }
    if (v & 0xff000000U) { ++ret; }

  }

  return ret;

}

/* Count the number of non-255 bytes set in the bitmap. Used strictly for the
   status screen, several calls per second or so. */

u32 count_non_255_bytes(afl_state_t *afl, u8 *mem) {

  u32 *ptr = (u32 *)mem;
  u32  i = ((afl->fsrv.real_map_size + 3) >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    /* This is called on the virgin bitmap, so optimize for the most likely
       case. */

    if (likely(v == 0xffffffffU)) { continue; }
    if ((v & 0x000000ffU) != 0x000000ffU) { ++ret; }
    if ((v & 0x0000ff00U) != 0x0000ff00U) { ++ret; }
    if ((v & 0x00ff0000U) != 0x00ff0000U) { ++ret; }
    if ((v & 0xff000000U) != 0xff000000U) { ++ret; }

  }

  return ret;

}

/* Destructively simplify trace by eliminating hit count information
   and replacing it with 0x80 or 0x01 depending on whether the tuple
   is hit or not. Called on every new crash or timeout, should be
   reasonably fast. */
const u8 simplify_lookup[256] = {

    [0] = 1, [1 ... 255] = 128

};

/* Destructively classify execution counts in a trace. This is used as a
   preprocessing step for any newly acquired traces. Called on every exec,
   must be fast. */

const u8 count_class_lookup8[256] = {

    [0] = 0,
    [1] = 1,
    [2] = 2,
    [3] = 4,
    [4 ... 7] = 8,
    [8 ... 15] = 16,
    [16 ... 31] = 32,
    [32 ... 127] = 64,
    [128 ... 255] = 128

};

u16 count_class_lookup16[65536];

void init_count_class16(void) {

  u32 b1, b2;

  for (b1 = 0; b1 < 256; b1++) {

    for (b2 = 0; b2 < 256; b2++) {

      count_class_lookup16[(b1 << 8) + b2] =
          (count_class_lookup8[b1] << 8) | count_class_lookup8[b2];

    }

  }

}

/* Import coverage processing routines. */

#ifdef WORD_SIZE_64
  #include "coverage-64.h"
#else
  #include "coverage-32.h"
#endif

/* Check if the current execution path brings anything new to the table.
   Update virgin bits to reflect the finds. Returns 1 if the only change is
   the hit-count for a particular tuple; 2 if there are new tuples seen.
   Updates the map, so subsequent calls will always return 0.

   This function is called after every exec() on a fairly large buffer, so
   it needs to be fast. We do this in 32-bit and 64-bit flavors. */

inline u8 has_new_bits(afl_state_t *afl, u8 *virgin_map) {

#ifdef WORD_SIZE_64

  u64 *current = (u64 *)afl->fsrv.trace_bits;
  u64 *virgin = (u64 *)virgin_map;

  u32 i = ((afl->fsrv.real_map_size + 7) >> 3);

#else

  u32 *current = (u32 *)afl->fsrv.trace_bits;
  u32 *virgin = (u32 *)virgin_map;

  u32 i = ((afl->fsrv.real_map_size + 3) >> 2);

#endif                                                     /* ^WORD_SIZE_64 */

  u8 ret = 0;
  while (i--) {

    if (unlikely(*current)) discover_word(&ret, current, virgin);

    current++;
    virgin++;

  }

  if (unlikely(ret) && likely(virgin_map == afl->virgin_bits))
    afl->bitmap_changed = 1;

  return ret;

}

/* A combination of classify_counts and has_new_bits. If 0 is returned, then the
 * trace bits are kept as-is. Otherwise, the trace bits are overwritten with
 * classified values.
 *
 * This accelerates the processing: in most cases, no interesting behavior
 * happen, and the trace bits will be discarded soon. This function optimizes
 * for such cases: one-pass scan on trace bits without modifying anything. Only
 * on rare cases it fall backs to the slow path: classify_counts() first, then
 * return has_new_bits(). */

inline u8 has_new_bits_unclassified(afl_state_t *afl, u8 *virgin_map) {

  /* Handle the hot path first: no new coverage */
  u8 *end = afl->fsrv.trace_bits + afl->fsrv.map_size;

#ifdef WORD_SIZE_64

  if (!skim((u64 *)virgin_map, (u64 *)afl->fsrv.trace_bits, (u64 *)end))
    return 0;

#else

  if (!skim((u32 *)virgin_map, (u32 *)afl->fsrv.trace_bits, (u32 *)end))
    return 0;

#endif                                                     /* ^WORD_SIZE_64 */
  classify_counts(&afl->fsrv);
  return has_new_bits(afl, virgin_map);

}

/* Compact trace bytes into a smaller bitmap. We effectively just drop the
   count information here. This is called only sporadically, for some
   new paths. */

void minimize_bits(afl_state_t *afl, u8 *dst, u8 *src) {

  u32 i = 0;

  while (i < afl->fsrv.map_size) {

    if (*(src++)) { dst[i >> 3] |= 1 << (i & 7); }
    ++i;

  }

}

#ifndef SIMPLE_FILES

/* Construct a file name for a new test case, capturing the operation
   that led to its discovery. Returns a ptr to afl->describe_op_buf_256. */

u8 *describe_op(afl_state_t *afl, u8 new_bits, size_t max_description_len) {

  u8 is_timeout = 0;

#if IGORFUZZ_FEATURE_ENABLE
  if (new_bits & 0x20) {

    new_bits -= 0x20;
    is_timeout = 1;

  }
#else
  if (new_bits & 0xf0) {

    new_bits -= 0x80;
    is_timeout = 1;

  }
#endif // IGORFUZZ_FEATURE_ENABLE

  size_t real_max_len =
      MIN(max_description_len, sizeof(afl->describe_op_buf_256));
  u8 *ret = afl->describe_op_buf_256;

  if (unlikely(afl->syncing_party)) {

    sprintf(ret, "sync:%s,src:%06u", afl->syncing_party, afl->syncing_case);

  } else {

    sprintf(ret, "src:%06u", afl->current_entry);

    if (afl->splicing_with >= 0) {

      sprintf(ret + strlen(ret), "+%06d", afl->splicing_with);

    }

    sprintf(ret + strlen(ret), ",time:%llu,execs:%llu",
            get_cur_time() + afl->prev_run_time - afl->start_time,
            afl->fsrv.total_execs);

    if (afl->current_custom_fuzz &&
        afl->current_custom_fuzz->afl_custom_describe) {

      /* We are currently in a custom mutator that supports afl_custom_describe,
       * use it! */

      size_t len_current = strlen(ret);
      ret[len_current++] = ',';
      ret[len_current] = '\0';

      ssize_t size_left = real_max_len - len_current - strlen(",+cov") - 2;
      if (is_timeout) { size_left -= strlen(",+tout"); }
      if (unlikely(size_left <= 0)) FATAL("filename got too long");

      const char *custom_description =
          afl->current_custom_fuzz->afl_custom_describe(
              afl->current_custom_fuzz->data, size_left);
      if (!custom_description || !custom_description[0]) {

        DEBUGF("Error getting a description from afl_custom_describe");
        /* Take the stage name as description fallback */
        sprintf(ret + len_current, "op:%s", afl->stage_short);

      } else {

        /* We got a proper custom description, use it */
        strncat(ret + len_current, custom_description, size_left);

      }

    } else {

      /* Normal testcase descriptions start here */
      sprintf(ret + strlen(ret), ",op:%s", afl->stage_short);

      if (afl->stage_cur_byte >= 0) {

        sprintf(ret + strlen(ret), ",pos:%d", afl->stage_cur_byte);

        if (afl->stage_val_type != STAGE_VAL_NONE) {

          sprintf(ret + strlen(ret), ",val:%s%+d",
                  (afl->stage_val_type == STAGE_VAL_BE) ? "be:" : "",
                  afl->stage_cur_val);

        }

      } else {

        sprintf(ret + strlen(ret), ",rep:%d", afl->stage_cur_val);

      }

    }

  }

  if (is_timeout) { strcat(ret, ",+tout"); }

#if IGORFUZZ_FEATURE_ENABLE
  switch (new_bits)
  {
  case 0x02:  strcat(ret, ",+cov"); break;
  case 0x11:  strcat(ret, ",-xxh"); break;
  case 0x12:  strcat(ret, ",-xcx"); break;
  case 0x13:  strcat(ret, ",-xch"); break;
  case 0x14:  strcat(ret, ",-bxx"); break;
  case 0x15:  strcat(ret, ",-bxh"); break;
  case 0x16:  strcat(ret, ",-bcx"); break;
  case 0x17:  strcat(ret, ",-bch"); break;
  default  :                        break;
  }
#else
  if (new_bits == 2) { strcat(ret, ",+cov"); }
#endif // IGORFUZZ_FEATURE_ENABLE

  if (unlikely(strlen(ret) >= max_description_len))
    FATAL("describe string is too long");

  return ret;

}

#endif                                                     /* !SIMPLE_FILES */

/* Write a message accompanying the crash directory :-) */

void write_crash_readme(afl_state_t *afl) {

  u8    fn[PATH_MAX];
  s32   fd;
  FILE *f;

  u8 val_buf[STRINGIFY_VAL_SIZE_MAX];

  sprintf(fn, "%s/crashes/README.txt", afl->out_dir);

  fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);

  /* Do not die on errors here - that would be impolite. */

  if (unlikely(fd < 0)) { return; }

  f = fdopen(fd, "w");

  if (unlikely(!f)) {

    close(fd);
    return;

  }

  fprintf(
      f,
      "Command line used to find this crash:\n\n"

      "%s\n\n"

      "If you can't reproduce a bug outside of afl-fuzz, be sure to set the "
      "same\n"
      "memory limit. The limit used for this fuzzing session was %s.\n\n"

      "Need a tool to minimize test cases before investigating the crashes or "
      "sending\n"
      "them to a vendor? Check out the afl-tmin that comes with the fuzzer!\n\n"

      "Found any cool bugs in open-source tools using afl-fuzz? If yes, please "
      "post\n"
      "to https://github.com/AFLplusplus/AFLplusplus/issues/286 once the "
      "issues\n"
      " are fixed :)\n\n",

      afl->orig_cmdline,
      stringify_mem_size(val_buf, sizeof(val_buf),
                         afl->fsrv.mem_limit << 20));      /* ignore errors */

  fclose(f);

}

#if IGORFUZZ_FEATURE_ENABLE

/**
 * Check if the current execution path brings anything few to the table.
 * It will change nothing but the virgin bits - reset a tuple to virgin
 * state (0xff) if not touched.
 * 
 * @return less than 0x10 ----> return as has_new_bits. Otherwise:
 * bms cov hcn  ---->  ret
 * 1   1   1    ---->  0x17
 * 1   1   0    ---->  0x16
 * 1   0   1    ---->  0x15
 * 1   0   0    ---->  0x14
 * 0   1   1    ---->  0x13
 * 0   1   0    ---->  0x12
 * 0   0   1    ---->  0x11
 * 0   0   0    ---->  0x10
*/
inline u8 has_few_bits(afl_state_t *afl, u8* virgin_map) {

  // There is no matrix, which means it is in process and
  // we should run normal has_new_bits to wait for its arrival.
  if (unlikely(!afl->testcase_matrix)) { return has_new_bits(afl, virgin_map); }

  u8 bms_decrease = 0; // whether bitmap_size gets decreased
  u8 cov_decrease = 0; // whether at least one edge is no longer hit
  u8 hcn_decrease = 0; // whether the total hit counts gets decreased
  
  u8 ret = 0x10; // ret < 0x10 means return as has_new_bits

#ifdef WORD_SIZE_64

  u64 *current = (u64 *)afl->fsrv.trace_bits;
  u64 *virgin  = (u64 *)virgin_map;

  u32 i = ((afl->fsrv.real_map_size + 7) >> 3);

#else

  u32 *current = (u32 *)afl->fsrv.trace_bits;
  u32 *virgin  = (u32 *)virgin_map;

  u32 i = ((afl->fsrv.real_map_size + 3) >> 2);

#endif // WORD_SIZE_64

  //====== check bitmap_size ======//
  u32 cur_bitmap_size = count_bytes(afl, afl->fsrv.trace_bits);
  if (cur_bitmap_size < afl->min_bitmap_size)
    bms_decrease = 1;

  //====== check whether any edge no longer hit ======//
  while (i--) {
    // Now we need to deal with two damn cases:
    // 1. (*virgin + 1) == 0
    //    The edges is not touched by testcase matrix. Since coverage-decrease 
    //    is going on, we only care about disappearance of already touched edges. 
    //    So ignore this case to optimize.
    // 2. (*current & *virgin) == 0
    //    It's just like which in discover_word - i.e., no bits in current bitmap 
    //    that have not been already cleared from the virgin map - this will almost 
    //    always be the case. But when an edge is not touched anymore, bit-AND also
    //    gives 0. We don't want to lose this since it's a typical coverage-decrease.
    //    So we sacrifice the speed optimization in exchange for hope :-(
#ifdef WORD_SIZE_64
    if (unlikely((u64)(*virgin + 1)))
#else
    if (unlikely((u32)(*virgin + 1)))
#endif
    {
      //====== check total hit counts ======//
      if (unlikely((*current & *virgin) && (afl->fsrv.actual_counts < afl->min_actual_cnts)))
        // trace_bits has already been classfied. We check if there are some bits 
        // that have not been already cleared from the virgin map to reduce sensitivity.
        hcn_decrease = 1;

      u8* cur = (u8*)current;
      u8* vir = (u8*)virgin;

#ifdef WORD_SIZE_64
      if ((vir[0] != 0xff && cur[0] == 0x0) || (vir[1] != 0xff && cur[1] == 0x0) ||
          (vir[2] != 0xff && cur[2] == 0x0) || (vir[3] != 0xff && cur[3] == 0x0) ||
          (vir[4] != 0xff && cur[4] == 0x0) || (vir[5] != 0xff && cur[5] == 0x0) ||
          (vir[6] != 0xff && cur[6] == 0x0) || (vir[7] != 0xff && cur[7] == 0x0))
      // If there is a touched byte being untouched now,
      // it will mean there is a path disappeared. Update virgin_bits here!
      {
        if (vir[0] != 0xff && cur[0] == 0x0) { vir[0] = 0xff; }
        if (vir[1] != 0xff && cur[1] == 0x0) { vir[1] = 0xff; }
        if (vir[2] != 0xff && cur[2] == 0x0) { vir[2] = 0xff; }
        if (vir[3] != 0xff && cur[3] == 0x0) { vir[3] = 0xff; }
        if (vir[4] != 0xff && cur[4] == 0x0) { vir[4] = 0xff; }
        if (vir[5] != 0xff && cur[5] == 0x0) { vir[5] = 0xff; }
        if (vir[6] != 0xff && cur[6] == 0x0) { vir[6] = 0xff; }
        if (vir[7] != 0xff && cur[7] == 0x0) { vir[7] = 0xff; }

        cov_decrease = 1;
      }
#else
      if ((vir[0] != 0xff && cur[0] == 0x0) || (vir[1] != 0xff && cur[1] == 0x0) ||
          (vir[2] != 0xff && cur[2] == 0x0) || (vir[3] != 0xff && cur[3] == 0x0))
      // If there is a touched byte being untouched now,
      // it will mean there is a path disappeared. Update virgin_bits here!
      {
        if (vir[0] != 0xff && cur[0] == 0x0) { vir[0] = 0xff; }
        if (vir[1] != 0xff && cur[1] == 0x0) { vir[1] = 0xff; }
        if (vir[2] != 0xff && cur[2] == 0x0) { vir[2] = 0xff; }
        if (vir[3] != 0xff && cur[3] == 0x0) { vir[3] = 0xff; }

        cov_decrease = 1;
      }
#endif // WORD_SIZE_64
    }
    current++;
    virgin++;
  } // while (i--)

  if (cov_decrease) afl->bitmap_changed = 1;

  ret += bms_decrease<<2;
  ret += cov_decrease<<1;
  ret += hcn_decrease;
  return ret; // should be in [0x10 , 0x17]
}

#include "sym-blacklist.inc"
/**
 * Read call stack file (if exists) generated
 * within __asan_on_error in the target, read
 * and check it to figure out the crash site.
 * This can help to check if interesting.
 * 
 * @attention The crash site should locate at
 * address (*offset) in virtual memory 
 * (before relocating if built as PIE, of course)
 * of the executable file located at path (*module),
 * However any of the three values could be 0,
 * due to those damn accidents behind.
 * So carefully check them before using!
 * 
 * @warning (*module) and (*symbol) will be
 * allocated by ck_strdup. Outside user 
 * should be responsible for calling ck_free
 * on them. WTF :-(
 * 
 * @param afl Need it to access call stack file.
 * @param flush If not 0, flush call stack file
 * with an empty line after everything done
 * (regardless of successful or failed parsing).
 * @param symbol Receive the symbol. Will be the
 * innermost one if inlined functions exist.
 * @param module Receive path to the module.
 * @param offset Receive offset in the module.
*/
void __attribute__((hot))
find_crash_site(afl_state_t *afl, u8 flush,
  u8 **symbol, u8 **module, u32 *offset) {
      *symbol = 0; *module = 0; *offset = 0; //safe init

  if (unlikely(!afl->fsrv.call_stack_file)) { return; }

  FILE *fp = fopen(afl->fsrv.call_stack_file, "r+");
  if (unlikely(!fp)) { return; }

  //no need to be all zero.
  char *line_buf = ck_alloc_nozero(IGORFUZZ_CALLSTACK_MAX_LINELEN);
  if (unlikely(!line_buf)) { return; }

  while (fgets(line_buf, IGORFUZZ_CALLSTACK_MAX_LINELEN, fp))
  {
    size_t len_ = strlen(line_buf);
    //Remove the newline character from a normal line or jump over.
    if (likely(len_ > IGORFUZZ_CALLSTACK_MIN_LINELEN && 
      line_buf[len_ - 1] == '\n')) {
      line_buf[len_ - 1]  = '\0';
      --len_;
    } else { continue; }

    char *path_ = strstr(line_buf, IGORFUZZ_CALLSTACK_FORMAT_PATH);
    if (unlikely(!path_)) continue; //field missing
    path_ += strlen(IGORFUZZ_CALLSTACK_FORMAT_PATH);

    char *addr_str = strstr(line_buf, IGORFUZZ_CALLSTACK_FORMAT_ADDR);
    if (unlikely(!addr_str || path_ >= addr_str)) continue; //field missing or corrupted

    *addr_str = '\0'; //all before belong to path
    addr_str += strlen(IGORFUZZ_CALLSTACK_FORMAT_ADDR);

    //check if it is a blocked module
    u8  module_blocked = 0;
    u8 *module_name = strrchr(path_, '/');
    if (unlikely(!module_name)) module_name = path_;
    //stack frame on a blocked module and the frames
    //on the top of it all shouldn't be crash site.
#if IGORFUZZ_CALLSTACK_EXACT_MODULE
    u8 *module_this = strrchr(afl->fsrv.target_path, '/');
    if (unlikely(!module_this)) module_this = afl->fsrv.target_path;
    if (unlikely(strcmp(module_this, module_name)))
      module_blocked = 1;
#else
    for (int i=0; sym_blacklist_module[i] != NULL; ++i) {
      if (strstr(module_name, sym_blacklist_module[i]))
        { module_blocked = 1; break; }
    }
#endif
    if (module_blocked) {
      //drop anything found previously
      ck_free(*symbol); *symbol = 0; //ck_free allows NULL input
      ck_free(*module); *module = 0; *offset = 0; 
      continue; //go for next stack frame
    }
    //module isn't a blocked one, so we check symbol next
    u32 addr_val = (u32)strtoul(addr_str, NULL, 16);

    unsigned long n_sym = 0, li_, co_;
    u8 *func = 0, *f_;  //so many damn stuffs!
    //Symbolize it anyway. We'll check later.
    SanSymTool_addr_send(path_, addr_val, &n_sym);
    SanSymTool_addr_read(0, (char**)(&f_), (char**)(&func), &li_, &co_);

    if (n_sym) {
      //If n_sym is non-zero, the module should be likely 
      //an analyzable stuff where crash site may locate.
      //So we set the crash site if not found previously.
      //If the symbol is proved to be blocked later, 
      //we'll remove it.
      if (func) {
        //If func isn't null, the symbolizer seems 
        //worked and had told sth valid about the symbol.
        //Next we are going to check the symbol.
        u8 func_blocked = 0;
        for (int i=0; sym_blacklist_function[i] != NULL; ++i) {
          if (strstr(func, sym_blacklist_function[i]))
            //stack frames on and on the top of a blocked
            //function all shouldn't be crash site.
            { func_blocked = 1; break; }
        }
        if (func_blocked) {
          //drop anything found previously
          ck_free(*symbol); *symbol = 0;
          ck_free(*module); *module = 0; *offset = 0;
        } else {
          //set all three fields
          if (!(*module)) {
            *symbol = ck_strdup(func);
            *module = ck_strdup(path_); *offset = addr_val;
          }
        }
      } else {
        //If func is null, we still firmly believe that this is 
        //very likely the crash site, but it just can't be symbolized.
        //So we only set module and offset.
        if (!(*module)) {
          *symbol = 0;
          *module = ck_strdup(path_); *offset = addr_val;
        }
      }
    } // if (n_sym) END
    SanSymTool_addr_free();
  } // while (fgets(...)) END

  ck_free(line_buf);
  if (flush) { rewind(fp); fprintf(fp, "\n"); }
  fclose(fp);
}

/**
 * Write README.txt in the crash directory for queued entry `q`
*/
void __attribute__((hot))
write_crash_detail(afl_state_t *afl, struct queue_entry *q) {

  u8 fn[PATH_MAX];
  sprintf(fn, "%s/crashes/README.txt", afl->out_dir);

  FILE *f = fopen(fn, "a");
  if (unlikely(!f)) { PFATAL("Failed to open %s", fn); }

  fprintf(f, "@FILE:%s; @SIZE:%x; @HITS:%llx; ",
    q->fname,
    q->bitmap_size,
    afl->fsrv.actual_counts
  );

  if (afl->crash_mode >= IGORFUZZ_NEW_CRASH_MODE_LV1) {
    if (afl->fsrv.crash_module) {
      fprintf(f, "@ADDR:%s+0x%x; ", afl->fsrv.crash_module, afl->fsrv.crash_offset);
    } else {
      fprintf(f, "@ADDR:%s; ", IGORFUZZ_CALLSTACK_NUL_TEXT);
    }
  }
  if (afl->crash_mode >= IGORFUZZ_NEW_CRASH_MODE_LV2) {
    if (afl->fsrv.crash_symbol) {
      fprintf(f, "@FUNC:%s; ", afl->fsrv.crash_symbol);
    } else {
      fprintf(f, "@FUNC:%s; ", IGORFUZZ_CALLSTACK_NUL_TEXT);
    }
  }

  fprintf(f, "\n");
  fclose(f);
}

/**
 * Compare current crash site with testcase matrix.
 * 
 * @param q It can be NULL, which makes *detail* and *discard*
 * don't take any effect.
 * @param detail If not 0, call write_crash_detail 
 * when crash site is different.
 * @param discard If not 0, will mark `q` as disabled in queue
 * when crash site is different.
 * @return 1 for same. 0 for different.
*/
u8 __attribute__((hot))
same_crash_site(afl_state_t *afl, struct queue_entry *q, u8 detail, u8 discard) {
  u8 is_same = 0;

  u8 *tmp_sym = afl->fsrv.crash_symbol;
  u8 *tmp_mod = afl->fsrv.crash_module;
  u32 tmp_ofs = afl->fsrv.crash_offset;

  find_crash_site(afl, 1, 
    &(afl->fsrv.crash_symbol),
    &(afl->fsrv.crash_module),
    &(afl->fsrv.crash_offset));

  if (unlikely((tmp_ofs != afl->fsrv.crash_offset)
                /* if one is null and the other not */
          ||   (!(tmp_mod && afl->fsrv.crash_module) && 
                  (tmp_mod || afl->fsrv.crash_module))
                /* We must test for null before strcmp
                  to avoid undefined behavior */
          ||   (tmp_mod && afl->fsrv.crash_module && 
                strcmp(tmp_mod, afl->fsrv.crash_module))
    )) //God damn it!!!!!!
  { is_same = 0; } else { is_same = 1; }

  if (likely(is_same)) {

    // Just keep the new allocated and
    // free the old ones for convenience.
    ck_free(tmp_sym);
    ck_free(tmp_mod);

  } else {

    if (q) {
      if (detail) {
        write_crash_detail(afl, q);
      }
      if (discard) {
        q->disabled = 1;
        q->perf_score = 0;
        if (!q->was_fuzzed) {
          q->was_fuzzed = 1;
          --afl->pending_not_fuzzed;
          --afl->active_items;
        }
      }
    } // if (q)

    ck_free(afl->fsrv.crash_symbol);
    afl->fsrv.crash_symbol = tmp_sym;
    ck_free(afl->fsrv.crash_module);
    afl->fsrv.crash_module = tmp_mod;
    afl->fsrv.crash_offset = tmp_ofs;

  }

  return is_same;
}

/**
 * Check if the result of an execve() during routine fuzzing is interesting.
 * Save or queue the input test case for further analysis if so.
 * 
 * @note afl->non_instrumented_mode should always be 0 when using IgorFuzz.
 * 
 * @return Returns 1 if entry is saved, 0 otherwise.
*/
u8 __attribute__((hot))
save_if_interesting(afl_state_t *afl, void *mem, u32 len, u8 fault) {

  if (unlikely(len == 0)) { return 0; }
  if (unlikely(fault == FSRV_RUN_TMOUT && afl->afl_env.afl_ignore_timeouts)) { return 0; }

  u8  fn[PATH_MAX];
  s32 fd;
  u8 *queue_fn = "";
  u8  classified = 0; u64 cksum = 0; //help afl->schedule
  u8  is_timeout = 0, few_bits = 0; //state store before function return
  u8  res;

  /* Update path frequency. */

  /* Generating a hash on every input is super expensive. Bad idea and should
     only be used for special schedules */
  if (unlikely(afl->schedule >= FAST && afl->schedule <= RARE)) {

    classify_counts(&afl->fsrv);
    classified = 1;

    cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

    /* Saturated increment */
    if (likely(afl->n_fuzz[cksum % N_FUZZ_SIZE] < 0xFFFFFFFF))
      afl->n_fuzz[cksum % N_FUZZ_SIZE]++;

  }

  while (1) { switch (fault) {
    case FSRV_RUN_TMOUT:
    ////////////////////
      //Timeouts are not very interesting, but we're still obliged to keep
      //a handful of samples. We use the presence of new bits in the
      //hang-specific bitmap as a signal of uniqueness.
      ++afl->total_tmouts;

      if (afl->saved_hangs >= KEEP_UNIQUE_HANG) { return 0; }

      if (unlikely(!classified)) { classify_counts(&afl->fsrv); classified = 1; }
      simplify_trace(afl, afl->fsrv.trace_bits);

      few_bits = has_few_bits(afl, afl->virgin_tmout);
      if (few_bits == 0x10 || few_bits == 0x00) { return 0; }

      //It's an interesting timeout. Go to save it.
      is_timeout = 0x20;
      //Before saving, we make sure that it's a genuine hang by re-running
      //the target with a more generous timeout (unless the default timeout is already generous).
      if (afl->fsrv.exec_tmout < afl->hang_tmout) {

        u32 tmp_len = write_to_testcase(afl, &mem, len, 0);
        if (likely(tmp_len)) { len = tmp_len;                              } 
        else                 { len = write_to_testcase(afl, &mem, len, 1); }

        fault = fuzz_run_target(afl, &afl->fsrv, afl->hang_tmout);
        classify_counts(&afl->fsrv);

        //A corner case that one user reported bumping into:
        //  increasing the timeout actually uncovers a crash. 
        //We restart the switch in while loop to make sure 
        //we don't discard it if so.
        if (fault == FSRV_RUN_CRASH) { continue; }
        //If not a timeout, also restart.
        if (fault != FSRV_RUN_TMOUT) { continue; }
      }

      snprintf(fn, PATH_MAX, "%s/hangs/id:%06llu,%s", afl->out_dir, afl->saved_hangs,
                describe_op(afl, 0, NAME_MAX - strlen("id:000000,")));

      fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
      if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn); }
      ck_write(fd, mem, len, fn);
      close(fd);

      ++afl->saved_hangs;
      afl->last_hang_time = get_cur_time();

      break;

    case FSRV_RUN_CRASH:
    ////////////////////
      //Since IgorFuzz is based on Crash Exploration Mode,
      //each queue entry should also be a crash.
      //Crashes which lead to coverage-decrease is what IgorFuzz needs.
      //Keep them and add them to queue. Now dir "crashes" is used for
      //holding "README.TXT" only, which saves more details for each queue entry.
      ++afl->total_crashes;

      if (unlikely(afl->crash_mode >= IGORFUZZ_NEW_CRASH_MODE_LV3)) {
        if (unlikely(!same_crash_site(afl, NULL, 0, 0)))
          return 0; // Crash site changed. Discard this case.
      }

      if (likely(classified)) {

        few_bits = has_few_bits(afl, afl->virgin_bits); 

      } else {

        classify_counts(&afl->fsrv);    classified = 1;
        few_bits = has_few_bits(afl, afl->virgin_bits);

      }

      if (likely(few_bits == 0x10 || few_bits == 0x00)) { return 0; }

      queue_fn = alloc_printf("%s/queue/id:%06u,%s", afl->out_dir, afl->queued_items,
          describe_op(afl, few_bits + is_timeout, NAME_MAX - strlen("id:000000,")));

      fd = open(queue_fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
      if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", queue_fn); }
      ck_write(fd, mem, len, queue_fn);
      close(fd);
      //After this afl->queue_top will points to the entry just added
      add_to_queue(afl, queue_fn, len, 0);

      if (few_bits & 0x02) {
        // this means +cov or -cov
        afl->queue_top->has_new_cov = 1;
        ++afl->queued_with_cov;
      }

      // due to classify counts we have to recalculate the checksum
      afl->queue_top->exec_cksum =
          hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      // For AFLFast schedules we update the new queue entry
      if (likely(cksum)) {
        afl->queue_top->n_fuzz_entry = cksum % N_FUZZ_SIZE;
        afl->n_fuzz[afl->queue_top->n_fuzz_entry] = 1;
      }

      // Try to calibrate inline; this also calls update_bitmap_score() when successful
      res = calibrate_case(afl, afl->queue_top, mem, afl->queue_cycle - 1, 0);

      if (unlikely(res == FSRV_RUN_ERROR))
        { FATAL("Unable to execute target application"); }

      if (likely(afl->q_testcase_max_cache_size)) 
        { queue_testcase_store_mem(afl, afl->queue_top, mem); }

      // We arrive here because has_few_bits indicates that queue entry
      // `afl->queue_top` is interesting. With calibrate_case run before
      // we have acquired better info and updated those global minimums. 
      // Now it's time to save the detail of current entry.
      u8 crash_detail_saved = 0;
      if (unlikely(afl->crash_mode)) {
        if (unlikely(afl->crash_mode >= IGORFUZZ_NEW_CRASH_MODE_LV3))
        {
          //If arrive here, the crash site should keep same.
          //However we still do double check in case it changed
          //because the testcase has been calibrated before.
          crash_detail_saved = 
            same_crash_site(afl, afl->queue_top, 1, 0) ? 0 : 1;
        }
        else if (likely(afl->crash_mode < IGORFUZZ_NEW_CRASH_MODE_LV3))
        {
          ck_free(afl->fsrv.crash_symbol);
          ck_free(afl->fsrv.crash_module);

          find_crash_site(afl, 1, 
            &(afl->fsrv.crash_symbol),
            &(afl->fsrv.crash_module),
            &(afl->fsrv.crash_offset));
        }
      }
      if (likely(!crash_detail_saved))
        write_crash_detail(afl, afl->queue_top);

      ++afl->saved_crashes;

      afl->last_crash_time = get_cur_time();
      afl->last_crash_execs = afl->fsrv.total_execs;

#ifdef __linux__
      if (afl->fsrv.nyx_mode) {

        u8 fn_log[PATH_MAX];

        (void)(snprintf(fn_log, PATH_MAX, "%s.log", fn) + 1);
        fd = open(fn_log, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
        if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn_log); }

        u32 nyx_aux_string_len = afl->fsrv.nyx_handlers->nyx_get_aux_string(
            afl->fsrv.nyx_runner, afl->fsrv.nyx_aux_string, 0x1000);

        ck_write(fd, afl->fsrv.nyx_aux_string, nyx_aux_string_len, fn_log);
        close(fd);

      }
#endif

      return 1;

    case FSRV_RUN_ERROR:
    ////////////////////
      FATAL("Unable to execute target application"); // exit() will be called
    default:
    ////////
      break;
  } /* switch (fault) END */ break; } /* while (1) END */
  return 0;
}


#else // IGORFUZZ_FEATURE_ENABLE


/* Check if the result of an execve() during routine fuzzing is interesting,
   save or queue the input test case for further analysis if so. Returns 1 if
   entry is saved, 0 otherwise. */

u8 __attribute__((hot))
save_if_interesting(afl_state_t *afl, void *mem, u32 len, u8 fault) {

  if (unlikely(len == 0)) { return 0; }

  if (unlikely(fault == FSRV_RUN_TMOUT && afl->afl_env.afl_ignore_timeouts)) {

    return 0;

  }

  u8  fn[PATH_MAX];
  u8 *queue_fn = "";
  u8  new_bits = 0, keeping = 0, res, classified = 0, is_timeout = 0,
     need_hash = 1;
  s32 fd;
  u64 cksum = 0;

  /* Update path frequency. */

  /* Generating a hash on every input is super expensive. Bad idea and should
     only be used for special schedules */
  if (unlikely(afl->schedule >= FAST && afl->schedule <= RARE)) {

    classify_counts(&afl->fsrv);
    classified = 1;
    need_hash = 0;

    cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

    /* Saturated increment */
    if (likely(afl->n_fuzz[cksum % N_FUZZ_SIZE] < 0xFFFFFFFF))
      afl->n_fuzz[cksum % N_FUZZ_SIZE]++;

  }

  if (likely(fault == afl->crash_mode)) {

    /* Keep only if there are new bits in the map, add to queue for
       future fuzzing, etc. */

    if (likely(classified)) {

      new_bits = has_new_bits(afl, afl->virgin_bits);

    } else {

      new_bits = has_new_bits_unclassified(afl, afl->virgin_bits);

      if (unlikely(new_bits)) { classified = 1; }

    }

    if (likely(!new_bits)) {

      if (unlikely(afl->crash_mode)) { ++afl->total_crashes; }
      return 0;

    }

  save_to_queue:

#ifndef SIMPLE_FILES

    queue_fn =
        alloc_printf("%s/queue/id:%06u,%s", afl->out_dir, afl->queued_items,
                     describe_op(afl, new_bits + is_timeout,
                                 NAME_MAX - strlen("id:000000,")));

#else

    queue_fn =
        alloc_printf("%s/queue/id_%06u", afl->out_dir, afl->queued_items);

#endif                                                    /* ^!SIMPLE_FILES */
    fd = open(queue_fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
    if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", queue_fn); }
    ck_write(fd, mem, len, queue_fn);
    close(fd);
    add_to_queue(afl, queue_fn, len, 0);

#ifdef INTROSPECTION
    if (afl->custom_mutators_count && afl->current_custom_fuzz) {

      LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

        if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

          const char *ptr = el->afl_custom_introspection(el->data);

          if (ptr != NULL && *ptr != 0) {

            fprintf(afl->introspection_file, "QUEUE CUSTOM %s = %s\n", ptr,
                    afl->queue_top->fname);

          }

        }

      });

    } else if (afl->mutation[0] != 0) {

      fprintf(afl->introspection_file, "QUEUE %s = %s\n", afl->mutation,
              afl->queue_top->fname);

    }

#endif

    if (new_bits == 2) {

      afl->queue_top->has_new_cov = 1;
      ++afl->queued_with_cov;

    }

    if (unlikely(need_hash && new_bits)) {

      /* due to classify counts we have to recalculate the checksum */
      afl->queue_top->exec_cksum =
          hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);
      need_hash = 0;

    }

    /* For AFLFast schedules we update the new queue entry */
    if (likely(cksum)) {

      afl->queue_top->n_fuzz_entry = cksum % N_FUZZ_SIZE;
      afl->n_fuzz[afl->queue_top->n_fuzz_entry] = 1;

    }

    /* Try to calibrate inline; this also calls update_bitmap_score() when
       successful. */
    res = calibrate_case(afl, afl->queue_top, mem, afl->queue_cycle - 1, 0);

    if (unlikely(res == FSRV_RUN_ERROR)) {

      FATAL("Unable to execute target application");

    }

    if (likely(afl->q_testcase_max_cache_size)) {

      queue_testcase_store_mem(afl, afl->queue_top, mem);

    }

    keeping = 1;

  }

  switch (fault) {

    case FSRV_RUN_TMOUT:

      /* Timeouts are not very interesting, but we're still obliged to keep
         a handful of samples. We use the presence of new bits in the
         hang-specific bitmap as a signal of uniqueness. In "non-instrumented"
         mode, we just keep everything. */

      ++afl->total_tmouts;

      if (afl->saved_hangs >= KEEP_UNIQUE_HANG) { return keeping; }

      if (likely(!afl->non_instrumented_mode)) {

        if (unlikely(!classified)) {

          classify_counts(&afl->fsrv);
          classified = 1;

        }

        simplify_trace(afl, afl->fsrv.trace_bits);

        if (!has_new_bits(afl, afl->virgin_tmout)) { return keeping; }

      }

      is_timeout = 0x80;
#ifdef INTROSPECTION
      if (afl->custom_mutators_count && afl->current_custom_fuzz) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

            const char *ptr = el->afl_custom_introspection(el->data);

            if (ptr != NULL && *ptr != 0) {

              fprintf(afl->introspection_file,
                      "UNIQUE_TIMEOUT CUSTOM %s = %s\n", ptr,
                      afl->queue_top->fname);

            }

          }

        });

      } else if (afl->mutation[0] != 0) {

        fprintf(afl->introspection_file, "UNIQUE_TIMEOUT %s\n", afl->mutation);

      }

#endif

      /* Before saving, we make sure that it's a genuine hang by re-running
         the target with a more generous timeout (unless the default timeout
         is already generous). */

      if (afl->fsrv.exec_tmout < afl->hang_tmout) {

        u8  new_fault;
        u32 tmp_len = write_to_testcase(afl, &mem, len, 0);

        if (likely(tmp_len)) {

          len = tmp_len;

        } else {

          len = write_to_testcase(afl, &mem, len, 1);

        }

        new_fault = fuzz_run_target(afl, &afl->fsrv, afl->hang_tmout);
        classify_counts(&afl->fsrv);

        /* A corner case that one user reported bumping into: increasing the
           timeout actually uncovers a crash. Make sure we don't discard it if
           so. */

        if (!afl->stop_soon && new_fault == FSRV_RUN_CRASH) {

          goto keep_as_crash;

        }

        if (afl->stop_soon || new_fault != FSRV_RUN_TMOUT) {

          if (afl->afl_env.afl_keep_timeouts) {

            ++afl->saved_tmouts;
            goto save_to_queue;

          } else {

            return keeping;

          }

        }

      }

#ifndef SIMPLE_FILES

      snprintf(fn, PATH_MAX, "%s/hangs/id:%06llu,%s", afl->out_dir,
               afl->saved_hangs,
               describe_op(afl, 0, NAME_MAX - strlen("id:000000,")));

#else

      snprintf(fn, PATH_MAX, "%s/hangs/id_%06llu", afl->out_dir,
               afl->saved_hangs);

#endif                                                    /* ^!SIMPLE_FILES */

      ++afl->saved_hangs;

      afl->last_hang_time = get_cur_time();

      break;

    case FSRV_RUN_CRASH:

    keep_as_crash:

      /* This is handled in a manner roughly similar to timeouts,
         except for slightly different limits and no need to re-run test
         cases. */

      ++afl->total_crashes;

      if (afl->saved_crashes >= KEEP_UNIQUE_CRASH) { return keeping; }

      if (likely(!afl->non_instrumented_mode)) {

        if (unlikely(!classified)) {

          classify_counts(&afl->fsrv);
          classified = 1;

        }

        simplify_trace(afl, afl->fsrv.trace_bits);

        if (!has_new_bits(afl, afl->virgin_crash)) { return keeping; }

      }

      if (unlikely(!afl->saved_crashes) &&
          (afl->afl_env.afl_no_crash_readme != 1)) {

        write_crash_readme(afl);

      }

#ifndef SIMPLE_FILES

      snprintf(fn, PATH_MAX, "%s/crashes/id:%06llu,sig:%02u,%s", afl->out_dir,
               afl->saved_crashes, afl->fsrv.last_kill_signal,
               describe_op(afl, 0, NAME_MAX - strlen("id:000000,sig:00,")));

#else

      snprintf(fn, PATH_MAX, "%s/crashes/id_%06llu_%02u", afl->out_dir,
               afl->saved_crashes, afl->fsrv.last_kill_signal);

#endif                                                    /* ^!SIMPLE_FILES */

      ++afl->saved_crashes;
#ifdef INTROSPECTION
      if (afl->custom_mutators_count && afl->current_custom_fuzz) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (afl->current_custom_fuzz == el && el->afl_custom_introspection) {

            const char *ptr = el->afl_custom_introspection(el->data);

            if (ptr != NULL && *ptr != 0) {

              fprintf(afl->introspection_file, "UNIQUE_CRASH CUSTOM %s = %s\n",
                      ptr, afl->queue_top->fname);

            }

          }

        });

      } else if (afl->mutation[0] != 0) {

        fprintf(afl->introspection_file, "UNIQUE_CRASH %s\n", afl->mutation);

      }

#endif
      if (unlikely(afl->infoexec)) {

        // if the user wants to be informed on new crashes - do that
#if !TARGET_OS_IPHONE
        // we dont care if system errors, but we dont want a
        // compiler warning either
        // See
        // https://stackoverflow.com/questions/11888594/ignoring-return-values-in-c
        (void)(system(afl->infoexec) + 1);
#else
        WARNF("command execution unsupported");
#endif

      }

      afl->last_crash_time = get_cur_time();
      afl->last_crash_execs = afl->fsrv.total_execs;

      break;

    case FSRV_RUN_ERROR:
      FATAL("Unable to execute target application");

    default:
      return keeping;

  }

  /* If we're here, we apparently want to save the crash or hang
     test case, too. */

  fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
  if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn); }
  ck_write(fd, mem, len, fn);
  close(fd);

#ifdef __linux__
  if (afl->fsrv.nyx_mode && fault == FSRV_RUN_CRASH) {

    u8 fn_log[PATH_MAX];

    (void)(snprintf(fn_log, PATH_MAX, "%s.log", fn) + 1);
    fd = open(fn_log, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);
    if (unlikely(fd < 0)) { PFATAL("Unable to create '%s'", fn_log); }

    u32 nyx_aux_string_len = afl->fsrv.nyx_handlers->nyx_get_aux_string(
        afl->fsrv.nyx_runner, afl->fsrv.nyx_aux_string, 0x1000);

    ck_write(fd, afl->fsrv.nyx_aux_string, nyx_aux_string_len, fn_log);
    close(fd);

  }

#endif

  return keeping;

}


#endif // IGORFUZZ_FEATURE_ENABLE