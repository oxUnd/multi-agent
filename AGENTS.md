# morph Agent Guide

## Build & Test
- CMake ≥ 3.20 required (README says 3.16 but `CMakeLists.txt` enforces 3.20)
- Build: `cmake -S . -B build && cmake --build build`
- Tests are ON by default (`BUILD_TESTS` defaults to ON); no flag needed
- Run all tests: `cd build && ctest --output-on-failure`
- Run a single test: `cd build && ctest -R test_arena --output-on-failure`
- Run test binary directly: `cd build && ./morph-tests --gtest_filter=TestArena*`
- ASAN build: `cmake -S . -B build -DENABLE_ASAN=ON && cmake --build build`
- FastCGI front-end: `cmake -S . -B build -DBUILD_FASTCGI=ON && cmake --build build`
- Clean build: `rm -rf build`

## Architecture Overview
- **ReAct loop**: Thought → Action → Observation → Guardrail → Final. Core in `src/agent/react.c`. Uses OpenAI Function Calling (not text parsing).
- **3 model backends**: `llm` (text chat), `image_gen`, `video_gen` — each configured independently in config.toml.
- **Tools**: built-in tools under `src/agent/tools/`: credits, memory, img_gen, img_inpaint, img_compose, img_info, img_resize, img_convert, img_annotate, vid_gen, file_read, file_list, file_info, bash_exec, skill_activate, plan (plus sub-agent tools: delegate, agent_status, fanout, and ask_user). Text generation and QA are handled directly by the language model. img_inpaint = bbox+label region generation (deterministic %-coordinate i2i instruction); img_compose = arrow+label cross-image fusion (local pre-composite + i2i harmonize). Both consume the img_annotate JSON verbatim.
- **Plan subsystem**: structured multi-step planning with status tracking. Code in `src/agent/plan.c`. Registered as `plan` tool.
- **MCP**: Model Context Protocol client (stdio + Streamable HTTP). Discovers and auto-registers remote tools/resources/prompts as morph tools. Code in `src/mcp/`. Config via `[[mcp.servers]]` in config.toml.
- **Skills**: hot-loadable instruction packs (`SKILL.md` with YAML frontmatter). Discovery from `~/.morph/skills/` and `~/.agents/skills/`. Code in `src/skill/`. Examples in `skills/`.
- **Exts**: hot-pluggable extensions via sandbox; live in `~/.morph/exts/`. Manifest format: TOML with `entry`, `permissions`, `args_schema`. Demo at `exts/demo-translate/`, `exts/demo-upper/`.
- **IPC**: JSON-RPC over stdin/stdout for ext subprocesses. Code in `src/ipc/`.
- **Context compression**: hierarchical fallback in `src/agent/compress.c`, triggered at `summarize_threshold_ratio` (default 0.8).
- **Sandbox**: seccomp+rlimit (Linux), sandbox-exec (macOS, P2 — not yet implemented). Code in `src/sandbox/`.

## Library Dependency Chain
All libraries are static. Derived from actual CMake link targets:
```
morph-toml (vendor/tomlc17.c)
  ↓
morph-util (arena, log, file, cJSON, base64, utf8, spin) ← base lib, cJSON compiled in
  ↓
morph-db (SQLite) ──→ morph-session ──→ morph-persistence
morph-persistence (memory/credit stores) ──→ morph-credits
morph-http (client, SSE: libcurl) ──→ morph-models (llm, image_gen, video_gen)
  ↓
morph-agent (react, context, compress, tokenizer, tool, plan) ← links morph-persistence, Threads
  ↓
morph-tools ← links morph-agent, morph-models, morph-http, morph-util, morph-skill, morph-sandbox
  ↓
morph-skill ← links morph-util, morph-agent
morph-sandbox ──→ morph-ext
morph-mcp ← links morph-util, morph-agent, morph-http
morph-render (markdown via md4c, image, video)
morph-config (TOML-based) ──→ morph-cli (main CLI lib)
morph-ipc (jsonrpc)
```
Entrypoint: `src/sapi/cli/main.c` → initializes logging, HTTP, config, then runs CLI via `cli_run()`.

## Vendor
Bundled in `vendor/`: cJSON.c/h, stb_image.h, stb_image_write.h, stb_image_resize2.h, toml.c/h, sheredom_utf8.h. Compiled as part of the project, **not** fetched separately.
md4c is **fetched** by CMake FetchContent (not in vendor/). stb_image_write/resize2 have heavy warning suppressions in `src/agent/tools/CMakeLists.txt`.

## UTF-8 Utilities
All UTF-8 operations must use the shared API in `src/util/utf8.h` — **never** hand-roll your own UTF-8 decode/encode/width logic. The header wraps [sheredom/utf8.h](https://github.com/sheredom/utf8.h) (vendored as `vendor/sheredom_utf8.h`, header-only, all functions `inline`) and adds project-specific extensions.

**Include**: `#include "util/utf8.h"` (from outside `src/util/`) or `#include "utf8.h"` (from within `src/util/`)

**sheredom primitives** (available after including the header — no extra link):
- `utf8codepoint(str, &cp)` — decode next codepoint, returns advanced pointer; `cp` is `utf8_int32_t`
- `utf8codepointcalcsize(str)` — byte length of next codepoint
- `utf8len(str)` / `utf8nlen(str, n)` — count codepoints
- `utf8valid(str)` / `utf8nvalid(str, n)` — validate, return NULL if valid
- `utf8dup(str)` / `utf8ndup(str, n)` — strdup with UTF-8 awareness
- `utf8size_lazy(str)` / `utf8nsize_lazy(str, n)` — byte length without NUL
- `utf8ncpy`, `utf8ncat`, `utf8cmp`, `utf8casecmp`, `utf8chr`, `utf8rchr`, `utf8str`, `utf8casestr`, etc.

**Project-specific extensions** (defined in `src/util/utf8.c`, link `morph-util`):
- `utf8_safe_len(s, max_bytes)` — largest length ≤ max_bytes that doesn't split a multi-byte sequence
- `utf8_dup_clamped(src, max_bytes)` — strdup with truncation; appends `"…(truncated)"` when cut
- `utf8_sanitize_into(dst, src, src_len)` — copy dropping malformed bytes and embedded NULs; returns bytes written
- `utf8_sanitize_inplace(s)` — in-place sanitize (removes invalid UTF-8)
- `utf8_cp_width(cp)` — display column width for a codepoint (uses `wcwidth()` + comprehensive fallback table covering CJK/emoji)
- `utf8_visible_len(s)` — total display column width of a string
- `utf8_skip_forward(s, chars)` — advance pointer past N codepoints
- `utf8_copy_vis(dst, dst_cap, src, max_vis)` — copy up to max_vis visible characters
- `utf8_is_cjk_cp(cp)` — check if a codepoint is CJK ideograph / wide symbol
- `utf8_truncate(s, max_bytes)` — find safe UTF-8 truncation point
- `utf8_display_width(s)` — convenience wrapper returning `int`

**Width table**: `utf8_cp_width` uses `cli.c`'s comprehensive table (0x1100–0x3FFFD CJK ranges + 0x1F000–0x1FAFF emoji ranges + zero-width combining marks). All former per-file CJK tables have been consolidated here.

## Core Data Structures
Shared foundational containers live in `src/util/` (linked via `morph-util`). **Prefer these over hand-rolled equivalents.** Do not reimplement growable buffers, dynamic arrays, or hash maps.

- **Arena** (`arena.h`) — bump-pointer allocator for scope-lived data. `arena_create`/`arena_destroy`, `arena_alloc` (zero-filled, aligned), `arena_alloc_aligned`, `arena_strdup`, `arena_reset` (reuse). Core structs (`react_context`, etc.) hold a `struct arena *`; allocate internal data from it instead of per-object `malloc`/`free`.
- **`morph_buf_t`** (`buf.h`) — growable byte/string builder. `morph_buf_init` (heap) / `morph_buf_init_arena`, `morph_buf_append`/`puts`/`putc`/`printf`/`vprintf`, `morph_buf_cstr` (NUL-terminated view), `morph_buf_str` (`morph_str_t`), `morph_buf_detach` (take ownership), `morph_buf_cleanup`. Use this for any variable-length string assembly — **never** fixed `char[N]` + `snprintf` accumulation.
- **`morph_array_t`** (`array.h`) — generic dynamic array (element size set at init). `morph_array_init` (heap, `MORPH_ARRAY_INIT_CAP` default) / `morph_array_init_arena`, `morph_array_push`/`push_n`/`pop`/`get`/`reserve`/`clear`, `morph_array_foreach(ptr, arr, type)`, `morph_array_cleanup`. Use instead of fixed-capacity C arrays when the count is unbounded. Note: `push` may realloc — don't hold element pointers across pushes.
- **`morph_strmap_t`** (`strmap.h`) — open-addressing string→`void *` hash map. `morph_strmap_init`/`cleanup`/`clear`, `morph_strmap_set`/`get`/`contains`/`remove`/`len`. Use for string-keyed lookups (e.g. tool registries).
- **`morph_str_t`** (`str.h`) — `{len, const char *}` string view (often arena-backed). `morph_strdup`/`strndup`, `morph_strcmp`/`strcasecmp`/`strncmp`, `morph_str_to_c`, `morph_str_chr`/`rchr`/`trim`, `MORPH_STRLIT`. Use for non-owning slices; cJSON values and most APIs still pass plain `const char *`.
- **`morph_queue_t`** (`queue.h`, `typedef struct morph_queue`) — intrusive doubly-linked list (embed the field in your struct; recover with `morph_queue_data`). Macro-based: `morph_queue_init`, `insert_head`/`insert_tail`, `remove`, `foreach`/`foreach_safe`, plus `sort`/`split`/`middle` helpers. Header-only.

## Dependencies
- **Required**: SQLite3, libcurl, CMake ≥ 3.20
- **Fetched by CMake**: md4c (v0.5.3 via FetchContent), GoogleTest (v1.14.0 via FetchContent)
- **Optional (auto-detected)**: readline (searched in /opt/homebrew, /usr/local, /usr; falls back to fgets)
- **Optional**: libseccomp (Linux sandbox)

## Configuration
- Default config path: `~/.morph/config.toml` (must create manually: `mkdir -p ~/.morph && cp config.toml.example ~/.morph/config.toml`)
- Override via `-c` / `--config` flag
- API keys read from env vars (`api_key_env` field) — never hardcode in config
- MCP servers configured via `[[mcp.servers]]` TOML array in config.toml (stdio or HTTP transport)
- Logs: `~/.morph/log/agent.log`
- Output dir defaults to `~/.morph/output`
- Debug: `MORPH_DEBUG=1` prints every HTTP request/response

## Error Handling
- **Error type**: `typedef int morph_err_t` (defined in `src/util/error.h`)
- **Convention**: `0` = success, `< 0` = error; **never** return bare `-1`
- **POSIX errno**: use for system-level errors (e.g., `-EINVAL`, `-ENOMEM`, `-ENOENT`, `-EIO`, `-EPERM`, `-ECANCELED`)
- **Application codes**: `enum morph_error` starting at `-257` (base `-256`), for domain-specific errors:
  - `MORPH_ERR_NOT_CONFIGURED` (-257): missing API key / model config
  - `MORPH_ERR_NOT_INITIALIZED` (-258): subsystem not initialized
  - `MORPH_ERR_API` (-259): remote API returned HTTP error
  - `MORPH_ERR_NETWORK` (-260): curl / connection failure
  - `MORPH_ERR_PARSE` (-261): JSON / TOML / YAML parse failure
  - `MORPH_ERR_PROTOCOL` (-262): response missing expected fields
  - `MORPH_ERR_DB` (-263): SQLite operation failure
  - `MORPH_ERR_FORMAT` (-264): invalid image / media format
  - `MORPH_ERR_PROCESSING` (-265): image resize / convert failure
  - `MORPH_ERR_SANDBOX` (-266): extension killed by sandbox
  - `MORPH_ERR_LOAD` (-267): dlopen / dlsym failure
  - `MORPH_ERR_LLM` (-268): LLM chat call failed
- **`MORPH_RETURN(code)`**: use instead of `return code;` for error returns — auto-logs `morph_strerror(code)` + `__FILE__:__LINE__` in debug mode, zero-overhead in release
- **`MORPH_SET_ERR(var, code)`**: when a centralized cleanup path is justified, use this before jumping to it so the error code is preserved and logged
- **`morph_strerror(err)`**: returns human-readable string for all error codes (POSIX + custom); use for LLM-facing and user-facing messages, never raw `%d`
- **System calls**: return `-errno` (not `-EIO`) when `pipe()`, `fork()`, `select()`, `opendir()` etc. fail

### `goto` Usage
Follow the Linux kernel's centralized-exit guidance, with a stronger project preference for avoiding `goto`:
- **Default: do not use `goto`.** Use direct returns for validation failures and error paths that need no cleanup.
- Use `goto` only when several exit paths share non-trivial cleanup and a centralized exit clearly reduces duplicated cleanup or excessive nesting. Do not use it for ordinary control flow.
- Before adding `goto`, prefer smaller helper functions, clearer ownership, or immediate local cleanup when those keep the code simple.
- Cleanup labels must describe the action, such as `out_free_buffer`; never use numbered labels such as `err1` or `err2`.
- For partially initialized resources, use distinct cleanup levels in reverse acquisition order. Never send every failure to one label that assumes all resources were initialized.
- A function using centralized cleanup must preserve its real error code with `MORPH_SET_ERR`/`MORPH_SET_ERRNO`; never fall through and return success after an error.
- Reference: [Linux kernel coding style §7, Centralized exiting of functions](https://www.kernel.org/doc/html/latest/process/coding-style.html#centralized-exiting-of-functions).

## C Coding Conventions (from REQUIREMENTS.md §6.11)
These differ from typical C defaults and must be followed:
- **No `//` comments** — C-style `/* */` only
- **Source strings**: use English for errors, logs, status messages, UI copy, and model instructions. Keep non-English text when it is input-matching data, a fixture, or a necessary example. UI localization belongs in a shared i18n layer, not ad hoc multilingual strings in business logic; keep translatable messages as complete templates.
- **`sizeof(var)`** not `sizeof(type)`
- **Use `<limits.h>` constants for system limits** — never hardcode sizes that correspond to POSIX/system limits:
  - Filesystem path buffers → `PATH_MAX` (not `512`/`1024`/`4096`)
  - stdio I/O buffers → `BUFSIZ` (not `4096`)
  - Do **not** use `NAME_MAX` for logical names (tool names, skill names, etc.) — those are application-defined, keep named `#define` constants like `TOOL_NAME_MAX`
  - Application-specific limits (e.g. `MAX_CONTENT_SIZE`, `ARENA_DEFAULT_SIZE`) stay as named `#define`s
  - `strncpy` must use `sizeof(dst) - 1`, never magic numbers like `63`
- **Error codes**: negative errno (`-EINVAL`, `-ENOMEM`) or `MORPH_ERR_*` from `src/util/error.h`; use `MORPH_RETURN(code)` instead of bare `return code;` for all error returns
- **Cleanup**: prefer direct returns with immediate local cleanup; use centralized `goto` cleanup only under the rules above; no return path may leak
- **Memory**: Arena (`arena_alloc`/`arena_strdup`) for scope-lived data; `morph_buf`/`morph_array` for growable strings/arrays (see Core Data Structures); raw `malloc`/`free` requires NULL checks and explicit cleanup on every exit path
- **Multi-statement macros**: wrapped in `do { } while (0)`
- **Naming**: functions `snake_case`, types `struct foo`, macros `UPPER_CASE`
- **Warnings**: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` — CI must pass with 0 warnings
- Tab indent (8 chars); soft limit 80 cols, hard limit 100

## Test Conventions
- Tests are C++17 (GoogleTest) linking C static libs
- Test files in `tests/` named `test_<module>.cpp`
- `test_ext_demo.c` exists but is **not** in the CMake test build — do not rely on it
- Integration tests use mock LLM (local HTTP server returning fixed SSE)
- Memory testing: Valgrind + ASan + UBSan expected clean

## Gotchas
- Configuration is parsed with vendored tomlc17 and checked against the embedded schema in `src/config/schema.c`
- `img_resize.c` and `img_convert.c` need extensive warning suppressions for stb headers (already in CMake)
- `config.toml` is gitignored (contains API keys); use `config.toml.example` as template
- `vendor/md4c/` is gitignored (fetched at build time)
- `.morph/` is gitignored (runtime data dir)
- macOS sandbox (`sandbox-exec`) is not yet implemented — only Linux seccomp is active
