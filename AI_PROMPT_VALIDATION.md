# AI_PROMPT local implementation and validation

Baseline: `5bce9e06d1281347ad61344e4b01fab9158c58a2`.
Branch: `competition/ai-prompt`.
Workspace: `/home/yhr/seekdb_competition/ai_competition_2026`.

## Changes

- `ob_expr_ai_prompt.cpp`: accept JSON without varchar casts; retain TEXT/ LONGTEXT calc types, normalize string calculation collation to UTF8MB4_BIN; read real LOB payloads using the existing helper; reject runtime NULL before get_string; propagate parser/allocation errors.
- `ob_ai_func_utils.cpp`: allow array arguments in prompt validation; print JSON containers, recursively render nested prompt objects (depth limited to 100); use two passes to size each occurrence exactly, bound output by INT32_MAX and reject overflowing/out-of-range numeric indexes. Inserted values are never scanned again.
- `ob_expr_ai_complete.cpp`: remove only the all-string Prompt-argument gate, so the enhanced Prompt renderer can consume structured arguments. No request, config, provider, network or response behavior was implemented for subtask 3.
- Added real-API Google Test source `unittest/sql/engine/expr/test_ob_expr_ai_prompt.cpp` and appended SQL/expected-output regressions to the existing ai_prompt mysqltest suite.
- Added `run_ai_prompt_local.py`, explicitly a local extracted-production-body test with replacement allocator/JSON/string APIs, not a database test.

## Existing semantics retained

AI_PROMPT returns a JSON object containing `template` and `args`; it does not eagerly expand the template. The existing SQL suite even constructs templates containing missing argument indexes. The consumer renderer checks indexes. Placeholders are decimal `{index}`. Malformed braces/nondecimal placeholders remain literal. There is no new backslash/double-brace escaping convention; SQL/JSON decoding performs its usual escaping before rendering.

## Executed checks

`python3 unittest/sql/engine/expr/run_ai_prompt_local.py`

Compiles verbatim baseline/current validator and renderer function bodies with:
`g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`.
Runs with `ASAN_OPTIONS=detect_leaks=1`.

Baseline: 17 checks, 5 failures; UBSan reports signed integer overflow on a long numeric index.
Working version: 17 checks, 0 failures, exit 0, no ASan/UBSan diagnostics.
Covers repeated/adjacent/different indexes, leading zero indexes, missing/out-of-range/overflow indexes, malformed literal braces, backslash literals, inserted placeholder text, Unicode, embedded NUL, empty arguments and containers, JSON serialization path, nested prompts, 1 MiB arguments (4 MiB output), 1 MiB template, invalid object and allocation failure.
These results do NOT validate the actual seekdb JSON parser, SQL typing, LOB conversion, execution context or allocator integration.

`git diff --check`: passed.

`./build.sh release`: configuration failed: cannot find suitable compiler (bundled toolchain not initialized).
`cmake -S . -B build_release -DOB_CC=/usr/bin/gcc -DOB_CXX=/usr/bin/g++`: compiler probe failed because the build uses Clang-only options and GCC9-NOTFOUND toolchain settings.

Direct production syntax check attempted with:
```
g++ -std=c++20 -fsyntax-only -Isrc -Isrc/query/api -Isrc/storage/api -Isrc/data_plane/api -Isrc/objit/include -Isrc/oblib -Isrc/oblib/easy/include -Isrc/oblib/easy -Ideps/oblib/src -Ibuild_release/generated src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp
```
Blocked by missing generated Rust bridge `config_bridge.h`, before checking the changed implementation. Earlier missing source headers were resolved by adding the proper module include paths.

## Not executed / remaining risks

- Full seekdb build and real Google Test execution: toolchain/dependencies/build generation unavailable. This fixed baseline does not include the listed original AI_PROMPT unit test or a unittest CMakeLists.txt; the new Google Test file must be registered in/restored with the real module test infrastructure before execution.
- mysqltest ai_prompt.test: no mysqltest binary or newly built isolated database instance. Existing deployment was not used or modified. Appended result rows are authored expectations, not recorded successful output.
- Full-engine ASan/UBSan, actual TEXT/LONGTEXT LOB/charset conversion, real JSON binary-tree ownership, and downstream AI_COMPLETE integration remain unverified.
- No OOM was observed; full compilation was blocked by dependencies/toolchain, not demonstrated to exceed memory.

The implementation targets all three scoring groups (4+3+3), but no actual competition score can be claimed. Recommend completing real build and isolated SQL regression before evaluation or beginning subtask 2.
No push, PR, evaluation trigger, private repository change, existing deployment change, telemetry change or PR #2455 change was made.
