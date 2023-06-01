# Salute oldest `Igor/IgorFuzz` & `Igor/analyzer` & `Igor/evaluation`

:sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles::star2::sparkles:

Things in this directory come from the original [HexHive/Igor](https://github.com/HexHive/Igor). Especially files in `./contrast` show you how the primitive IgorFuzz works. If you do not clearly understand these codes, please just watch but DO NOT use.

---

:bangbang::bangbang: Following comes from the old tutorials. Once again, all contents in `oldest` are for respecting and commemorating the pioneering work of the pioneers and for internal use in some special situations, so usually *watch but DO NOT use* should be your best choice.

---

## Chapter I

`collect_decreased_poc.sh` helps you collect the decreased pocs with the smallest bitmap size.
```console
$ collect_decreased_poc.sh -i /path/to/decreased/poc/dir -o /path/to/result/dir
```

## Chapter II

`AsanParser.py` and `bitmap_size_formatter.py` are scripts for internal use.

## Chapter III

We developed IgorFuzz based on AFLplusplus crash exploration mode. It can prune
the paths that unnecessary for bug triggering very fast. Before using IgorFuzz,
we suggest use afl-tmin to shrink the size of crash first, so that IgorFuzz will
have better performance. 

### Installation and Usage

The installation and usage of IgorFuzz is completely same to the AFLplusplus'
crash mode. Even time you want to launch IgorFuzz, you must confirm that you
have put a PoC in input directory and set up output directory properly.


### Reduction in parallel

IgorFuzz reduces one PoC at one time. To apply IgorFuzz on many PoCs parallelly,
we provide users with `mass_fuzz.sh`. It will automatically run over and over
again untill all PoCs in input dir are fuzzed.


Collect all PoCs you want to reduce in input directory(e.g., `/home/my_pocs`), and set up output dir(e.g., `/home/trimmed/my_pocs`). 

The third arg is the number of PoCs you want to fuzz parallelly each time. The
last arg is the duration the fuzzing last for(e.g., 1h2m3s).

Example:
```console
$ ./mass_fuzz.sh /home/my_pocs /home/trimmed/my_pocs 30 10h
```


The form of result is: `/home/trimmed/my_pocs/$the-name-of-a-PoC(like: id:000000,xxxxxxxx)/`


`mass_fuzz.sh` renames fuzzed PoCs like: `fzd_id:000000,xxxxx`. So if there's something wrong with IgorFuzz or you want to shirnk all PoCs again, you can use `./clear_fzd.sh $INPUT_DIR` to remove "fzd" prefix. 
