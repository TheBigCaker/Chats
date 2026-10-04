# drygon completeness report

Generated: Sat Oct  3 22:28:18 PDT 2026
Binary: /data/data/com.termux/files/home/CCP-C-Cython-Python/drygon/drygon.elf (sha256 00e7a1b311e81ef8bdf1c13a3e563a0eff14654abb9887fcfba6e283d9a6c891)
Method: each case compiled to a named .elf (no .ros cache), then run separately;
compile failures and program failures are counted separately.
Features: parallel execution (8 jobs), incremental caching, granular timeouts, JSON/JUnit output, resource isolation (unshare).

## Summary

| Metric | Value |
|--------|-------|
| Total tests | 112 |
| Compiled OK | 112 |
| Pass (correct stdout/exit) | 103 (91%) |
| Fail (wrong value) | 4 |
| Crash (compiler) | 0 |
| Crash (program) | 4 |
| Timeout (program) | 1 |
| C99 | 49 / 49 (100%) |
| Python | 54 / 63  (85%) |

## Failure layers

| Layer | Count | Description |
|-------|-------|-------------|
| Parse |  | Front end can't tokenize/accept syntax |
| Compile | 0 | Front end rejected valid construct |
| Codegen | 0 | Member offset / code generation error |
| Transpiler | 0 | Python->C transpiler gap |
| Linker | 0 | Unresolved symbols |
| Compiler crash | 0 | SIGSEGV/SIGABRT in drygon itself |
| Program crash | 4 | Compiled ok; program segfaulted |
| Program timeout | 1 | Compiled ok; program hung |
| Output | 4 | Compiled+ran; wrong value (check N failed) |

## C99 tests

| Test | Status | Layer | Exit | Detail |
|------|--------|-------|------|--------|
| 01_types.c | PASS | pass | 0/0 |  |
| 03_control_flow.c | PASS | pass | 0/0 |  |
| 02_operators.c | PASS | pass | 0/0 |  |
| 07_string.c | PASS | pass | 0/0 |  |
| 08_memory.c | PASS | pass | 0/0 |  |
| 05_preprocessor.c | PASS | pass | 0/0 |  |
| 04_functions.c | PASS | pass | 0/0 |  |
| 06_stdio.c | PASS | pass | 0/0 |  |
| 11_structs.c | PASS | pass | 0/0 |  |
| 13_enums.c | PASS | pass | 0/0 |  |
| 09_bool.c | PASS | pass | 0/0 |  |
| 14_storage.c | PASS | pass | 0/0 |  |
| 10_vla.c | PASS | pass | 0/0 |  |
| 15_pointers.c | PASS | pass | 0/0 |  |
| 17_unions.c | PASS | pass | 0/0 |  |
| 16_casts.c | PASS | pass | 0/0 |  |
| 18_initializers.c | PASS | pass | 0/0 |  |
| 19_switch.c | PASS | pass | 0/0 |  |
| 22_math.c | PASS | pass | 0/0 |  |
| 21_stdlib.c | PASS | pass | 0/0 |  |
| 23_limits.c | PASS | pass | 0/0 |  |
| 20_limits.c | PASS | pass | 0/0 |  |
| 25_ctype.c | PASS | pass | 0/0 |  |
| 24_stdint.c | PASS | pass | 0/0 |  |
| 26_string_lib.c | PASS | pass | 0/0 |  |
| 27_stdio_adv.c | PASS | pass | 0/0 |  |
| 28_complex.c | PASS | pass | 0/0 |  |
| 29_setjmp.c | PASS | pass | 0/0 |  |
| 30_signal.c | PASS | pass | 0/0 |  |
| 31_errno.c | PASS | pass | 0/0 |  |
| 33_time.c | PASS | pass | 0/0 |  |
| 32_assert.c | PASS | pass | 0/0 |  |
| 34_fenv.c | PASS | pass | 0/0 |  |
| 35_promotions.c | PASS | pass | 0/0 |  |
| 37_noreturn.c | PASS | pass | 0/0 |  |
| 38_variadic.c | PASS | pass | 0/0 |  |
| 36_restrict.c | PASS | pass | 0/0 |  |
| 40_translation.c | PASS | pass | 0/0 |  |
| 41_locale.c | PASS | pass | 0/0 |  |
| 39_compound_lit.c | PASS | pass | 0/0 |  |
| 42_wchar.c | PASS | pass | 0/0 |  |
| 44_iso646.c | PASS | pass | 0/0 |  |
| 45_tgmath.c | PASS | pass | 0/0 |  |
| 43_inttypes.c | PASS | pass | 0/0 |  |
| 46_string_literals.c | PASS | pass | 0/0 |  |
| 47_ub_edge.c | PASS | pass | 0/0 |  |
| 48_digraphs.c | PASS | pass | 0/0 |  |
| 49_initializer_edge.c | PASS | pass | 0/0 |  |
| 50_conversion.c | PASS | pass | 0/0 |  |

## Python tests

| Test | Status | Layer | Exit | Detail |
|------|--------|-------|------|--------|
| 01_int.py | PASS | pass | 0/0 |  |
| 02_float.py | PASS | pass | 0/0 |  |
| 03_str.py | PASS | pass | 0/0 |  |
| 07_set.py | PASS | pass | 0/0 |  |
| 05_dict.py | PASS | pass | 0/0 |  |
| 04_list.py | PASS | pass | 0/0 |  |
| 06_tuple.py | PASS | pass | 0/0 |  |
| 08_bool_none.py | PASS | pass | 0/0 |  |
| 09_operators.py | PASS | pass | 0/0 |  |
| 10_control.py | PASS | pass | 0/0 |  |
| 12_classes.py | PASS | pass | 0/0 |  |
| 15_comprehensions.py | PASS | pass | 0/0 |  |
| 13_builtins.py | PASS | pass | 0/0 |  |
| 17_string_ops.py | PASS | pass | 0/0 |  |
| 18_modules.py | PASS | pass | 0/0 |  |
| 14_methods.py | PASS | pass | 0/0 |  |
| 21_os.py | PASS | pass | 0/0 |  |
| 19_file_io.py | PASS | pass | 0/0 |  |
| 16_exceptions.py | PASS | pass | 0/0 |  |
| 11_functions.py | PASS | pass | 0/0 |  |
| 22_sys.py | PASS | pass | 0/0 |  |
| 24_collections.py | FAIL | output | 0/133 | wrong value: check 133 failed |
| 25_itertools.py | PASS | pass | 0/0 |  |
| 27_re.py | PASS | pass | 0/0 |  |
| 23_pathlib.py | PASS | pass | 0/0 |  |
| 26_functools.py | PASS | pass | 0/0 |  |
| 28_datetime.py | PASS | pass | 0/0 |  |
| 29_copy.py | PASS | pass | 0/0 |  |
| 31_hashlib.py | PASS | pass | 0/0 |  |
| 30_random.py | PASS | pass | 0/0 |  |
| 32_struct.py | PASS | pass | 0/0 |  |
| 33_io.py | PASS | pass | 0/0 |  |
| 34_abc.py | PASS | pass | 0/0 |  |
| 38_typing.py | PASS | pass | 0/0 |  |
| 37_dataclasses.py | PASS | pass | 0/0 |  |
| 35_async.py | PASS | pass | 0/0 |  |
| 36_match.py | PASS | pass | 0/0 |  |
| 39_descriptors.py | FAIL | output | 0/1 | wrong value: check 1 failed |
| 40_serialization.py | PASS | pass | 0/0 |  |
| 41_textwrap.py | PASS | pass | 0/0 |  |
| 42_calendar.py | PASS | pass | 0/0 |  |
| 43_bisect.py | PASS | pass | 0/0 |  |
| 44_array_mod.py | PASS | pass | 0/0 |  |
| 46_weakref.py | CRASH | prog-crash | 0/139 | program segfault/abort (rc=139) |
| 45_threading.py | PASS | pass | 0/0 |  |
| 46_weakref_i.py | CRASH | prog-crash | 0/139 | program segfault/abort (rc=139) |
| 47_contextlib.py | FAIL | output | 0/1 | wrong value: check 1 failed |
| 48_enum.py | PASS | pass | 0/0 |  |
| 52_unittest.py | PASS | pass | 0/0 |  |
| 53_memoryview.py | PASS | pass | 0/0 |  |
| 55_operator_mod.py | PASS | pass | 0/0 |  |
| 51_logging.py | PASS | pass | 0/0 |  |
| 49_subprocess.py | PASS | pass | 0/0 |  |
| 50_csv.py | PASS | pass | 0/0 |  |
| 54_string_adv.py | PASS | pass | 0/0 |  |
| 56_math_adv.py | PASS | pass | 0/0 |  |
| 57_nonlocal_global.py | PASS | pass | 0/0 |  |
| 58_metaclass.py | CRASH | prog-crash | 0/139 | program segfault/abort (rc=139) |
| simple_test.py | PASS | pass | 0/0 |  |
| 60_comprehensive.py | FAIL | output | 0/26 | wrong value: check 26 failed |
| test.py | PASS | pass | 0/0 |  |
| 59_pickle_adv.py | CRASH | prog-crash | 0/139 | program segfault/abort (rc=139) |
| 20_advanced.py | TIMEOUT | prog-timeout | 0/124 | program exceeded 60s |

## Repair priority

1. **Transpiler** (0 direct + most of the malformed-C parse errors) — set/dict/tuple literals in expression position, super(), del, match/case, tuple unpacking, literal method calls, module-attr chains
2. **Compiler crashes** (0) — 05_preprocessor/11_functions/47_contextlib segfault drygon itself
3. **Program crashes** (4) — 28_complex, 39_compound_lit, 22_sys, 51_logging
4. **Wrong values** (4) — unsigned wrap, INT_MIN literals, VLA, struct-by-value, digraphs
5. **Macro expansion** — && token vanishes in macro-argument substitution (32_assert)
