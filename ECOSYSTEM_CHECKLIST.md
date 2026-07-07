# Nari - Ecosystem Readiness Checklist

> What it takes for Nari to become a *proper language ecosystem* - not just a working
> interpreter, but something other people can trust, install, learn, extend, and ship with.
>
> This checklist was derived by auditing the actual codebase (~43.7k LOC in `src/`, the
> npkg CLI, the Go registry server, the web frontend, LSP/DAP, CI, release, and docs), **not**
> by copying `ROADMAP.md`. Items are unchecked because they are work still to do. Where the
> existing roadmap is now stale (e.g. rate limiting, `search`/`unpublish`/`owner` already
> exist), that is noted inline.

**How to read priorities**

- **P0 - Trust blockers.** Correctness/security bugs. A "proper" ecosystem cannot ship on top of these.
- **P1 - Credibility.** The things people expect from any real language (spec, stable stdlib, tests, docs, governance).
- **P2 - Maturity.** Polish, breadth, and convenience that separate "usable" from "pleasant."
- **P3 - Nice-to-have.** Reach goals.

---

## P0 - Correctness & security (fix before anything else)

These are concrete, code-verified defects. Sources cited inline.

- [x] **Fixed unmasked shift UB (was CONFIRMED, live).** All three paths now mask the shift count to `[0, 63]`: interpreter (`bytecode.cpp:1120,1129`), C++ helpers (`jit_helpers.cpp:332,339`), and the JIT inline path (`asmjit_jit.cpp:5041,5050`). Mask-to-63 chosen (defines the x86 de-facto behavior; zero observable change vs. the already-shipped inline JIT). Release suite green. - `JIT_UB_AUDIT.md` #1.
- [x] **Mitigated latent use-after-free in method JIT (still latent by design).** The vector whose `.data()`/`&vec[i]` get baked as immediates (`compiled_fn_vec`) is now sized exactly once in `reset_for_chunk()` and never grown; `assert_tables_stable()` re-checks the storage base hasn't moved immediately before every address bake. Asserts run in release too (`NDEBUG` unset), so the full suite exercised the invariant with no trip. Structural fix (stable-base indexing) deferred as not low-risk. - `src/jit/asmjit_jit.cpp`; `JIT_UB_AUDIT.md` #3.
- [x] **Partially fixed trace-JIT struct-layout + empty-frames OOB.** Empty-frames OOB **resolved**: `assert(!frames.empty())` at the single trace entry (`bytecode.cpp:1238`). The `VM::frames` member offset now derives via `field_offset()` instead of a hardcoded `sizeof(std::vector)+8` (`trace_jit_asmjit.cpp:129-131`). - `JIT_UB_AUDIT.md` #4.
  - Warning: Residual: the inner `_M_finish + sizeof(void*)` libstdc++ 3-pointer assumption remains (documented `TODO`, line 128); no low-risk portable accessor, so left for a dedicated portability pass.
- [x] **Removed the dead unsafe helper** (`jit_str_append_var_const`, mismatched stack effect): deleted outright - zero remaining references. The later IR-only cutover also removed the legacy fused append helper family. - `JIT_UB_AUDIT.md` #2.
- [x] **Removed legacy single-pass method JIT.** The method JIT now has one producer (`ir_compile`); non-IR-eligible functions return `nullptr` and run in the bytecode interpreter. The old `translate_and_compile()` verifier-precondition risk is gone with the function. - `src/jit/asmjit_jit.cpp`; `JIT_UB_AUDIT.md` #5.
- [x] **Implement `FloatEq` / `FloatNe` in the trace JIT.** Recorder now emits `FloatEq`/`FloatNe` (was an `abort_recording`); asmjit backend eagerly materializes a **NaN-correct** boolean (x86: combines ZF with PF via `sete`+`setnp`/`setne`+`setp`; ARM64: `cset EQ`/`NE`). NaN+finite regression test added (`tests/expect_pass/test_jit_float_eq_ne.nari`); full suite green in release, no regressions. - `src/bytecode/bytecode.cpp:2987`, `src/jit/trace_jit.h`, `src/jit/jit_arch.h` (`float_to_bool_eq`), `src/jit/trace_jit_asmjit.cpp`.
  - Warning: **Discovered (see P2 VM item): float comparisons feeding a branch get no trace-JIT acceleration** - output is correct (deopt fallback) but there's no speedup. Pre-existing; affects the whole relational family, not just these new ops.
- [x] **Enforced a real JWT secret in production.** `resolveJWTSecret()` now `log.Fatal`s on a missing, known-placeholder, or `<32`-char secret outside dev (gated on `NARI_ENV`), with dev-only warnings and a placeholder blocklist. - `npkg-frontend/server/main.go:162-204`.
- [x] **Fixed newly-found signed-left-shift UB in `fits_int48` (audit had wrongly "ruled out" as R3).** `(v << 16 >> 16) == v` on a signed `int64_t` is UB pre-C++20 for negative/overflowing `v`, and the build is `cpp_std=c++17` - not C++20 as the audit assumed. Replaced with the equivalent UB-free range check `v >= INT48_MIN && v <= INT48_MAX`; bit-identical result, release suite green. (`get_int()`'s decode was already safe - unsigned operand.) - `src/core_types.h`; `JIT_UB_AUDIT.md` R3.
- [ ] **Rotate the real GitHub OAuth client secret** currently sitting in the working-tree `.env` (file is correctly gitignored and was never committed, but the value is live and must be rotated). Ensure `.env.example` carries placeholders only. - `npkg-frontend/server/.env`.
- [x] **Verify uploaded archive integrity server-side.** `UploadVersion` computes `sha256-<hex>` of the upload, optionally validates a client-declared `integrity` form field (400 on malformed format / digest mismatch), and re-hashes the file **at rest** after `WriteFile` (500 + `os.Remove` on mismatch) so a corrupted/tampered archive can never be served. Client `publish.nari` sends the declared digest; unit tests cover FIPS SHA-256 vectors. - `npkg-frontend/server/handlers/packages.go` (`computeIntegrity`/`hashFileIntegrity`/`UploadVersion`), `npkg-frontend/npkg/lib/publish.nari`, `packages_integrity_test.go`.
- [x] **Added a UBSan/ASan CI job.** `build.sh --sanitize` / `run_tests.sh --sanitize` produce an ASan+UBSan build (`-Db_sanitize=address,undefined -Db_lundef=false`, still `c++17`) and run the full suite; new `.github/workflows/sanitize.yml` runs it on push/PR. Full suite green under sanitizers (71/71 expect_pass + 13/13 naric robustness + npkg smokes). `detect_leaks` and `detect_container_overflow` are off **by design** - the asmjit JIT writes `vm->stack`'s end pointer directly (`asmjit_jit.cpp`), bypassing libc++ container annotations, so the size annotation is intentionally stale; heap-overflow / UAF / stack-use-after-return and **all** of UBSan remain active. - `build.sh`, `run_tests.sh`, `.github/workflows/sanitize.yml`.

---

## P1 - Language core & semantics

- [ ] **Write a formal language specification** (grammar + evaluation semantics). Today `docs/` is tutorial-style (`01` - `19`) with **no** EBNF/grammar/spec file. This is the single biggest credibility gap.
- [ ] **Define and document the error model end-to-end** (thrown values vs `Result`/`Option`, stack traces, error types) and make it consistent across runtime + stdlib.
- [ ] **Exhaustiveness checking for `match`** (no exhaustiveness logic found in the parser/compiler).
- [ ] **Getters/setters / computed properties** (not implemented; only incidental "get base path" comments in `src/parser.cpp`).
- [ ] **Specify type-annotation semantics** - clarify what annotations guarantee at runtime (checked? erased?) and document it.
- [ ] **Document defined integer/float/shift/overflow semantics** as part of the spec (ties to the P0 shift fix).
- [ ] **Parser error-recovery audit** so all statement/expr paths produce good diagnostics rather than bailing.

## P1 - Standard library

The stdlib is split across `src/stdlib/std/*.nari` (prelude + 11 importable modules) at version `0.0.3`. Breadth still needs work but structure is in place.

- [x] **Split the monolith into modules** with a clear public/internal boundary. _(Done - `src/stdlib/std/prelude.nari` is auto-loaded and exposes the bare globals (Result/Option, Ok/Err/Some/None, String, Array, Object, fs, io, platform, process, math, net, http, JSON, system aggregate, yield, Spawn). Optional modules are imported via `import { X } from "std/<name>";` and live under `src/stdlib/std/{archive,collections,date,encoding,hash,logger,path,random,regex,url,uuid}.nari`. Embed pipeline: `tools/embed_std_modules.py` (manifest-driven, called from `meson.build`) generates `embedded_std_modules.cpp` with `nari_std_prelude_source()` + `nari_std_module_source(name)`; the parser intercepts `std/*` import specs in `resolve_include_path` (`src/parser.cpp`) and pulls source from the embedded table via the `<std>/<name>` virtual path.)_
- [ ] **Define stdlib stability tiers + versioning policy** (what's stable vs experimental) before 1.0.
- [x] **Date/time module** - `Date` global with `now`/`utc`/`parseIso`/`format`/`fromMs`; instances expose `year/month/day/hour/minute/second/millis/weekday/yearday/utc` + `toIsoString`/`format`/`addMs`/`diffMs`/`equals`. Backed by 5 native helpers (`__time_now_ms`, `__time_components`, `__time_from_components`, `__time_format`, `__time_parse_iso`); `__time_format` adds a `%L` token for 3-digit milliseconds; parser is UTC-deterministic (`+/-HH:MM`/`Z` offsets normalized). Covered by `tests/expect_pass/test_stdlib_date.nari` (epoch round-trip, ISO parse/format, leap day, year rollover, sub-second truncation, catchable `DateError`).
- [x] **Flesh out `math`** - add constants (`PI`, `E`), and `log`/`exp`/`pow`/`round`/`tan`/`atan`/`atan2`/`clamp` (currently only sqrt/abs/min/max/rand/sin/cos/floor/ceil). _(Done - added 4 native builtins (`__math_log/exp/atan/atan2`), wired `tan`, added `PI`/`E`/`round`/`clamp`/`log10`/`log2`; covered by `tests/expect_pass/test_stdlib_math.nari`.)_
- [x] **First-class collections** - `Map`/`Set`/ordered map/deque beyond raw array+object. _(Done - `Map` (insertion-ordered, JS-style API) and `Set` (Map-backed dedup) globals; covered by `tests/expect_pass/test_stdlib_collections.nari`.)_
- [x] **Encoding helpers** - base64/hex (only `sha256` exists in `Hash`). _(Done - `Hex` and `Base64` globals backed by native binary-safe builtins (`__hex_encode/decode`, `__base64_encode/decode`); covered by `tests/expect_pass/test_stdlib_encoding.nari` incl. bytes >127.)_
- [x] **Regex surface in stdlib** (expose/standardize whatever the runtime offers). _(Done - `Regex` global with `new`/`test`/`match`/`exec`/`replace`/`split`/`escape`, backed by `__regex_new` native validator (srell `SRELL_NO_THROW` -> `ecode()` check, catchable via `script_throw`); covered by `tests/expect_pass/test_stdlib_regex.nari`.)_
- [x] **URL** module in stdlib.
- [x] **Path module** in stdlib (path utilities previously lived only inside npkg). _(Done - `Path` global with `join`/`dirname`/`basename`/`extname`/`isAbsolute`/`normalize`/`resolve`/`relative`; covered by `tests/expect_pass/test_stdlib_path.nari`.)_
- [x] **Logging module** with levels. _(Done - `Logger` global with trace/debug/info/warn/error/fatal, configurable sink + timestamps + min-level; covered by `tests/expect_pass/test_stdlib_logger.nari`.)_
- [x] **Randomness + UUID** (seedable RNG, UUID v4). _(Done - `Random.create(seed?)` returns a seedable RNG (linear-congruential, deterministic); `uuid.v4()` for IDs; covered by `tests/expect_pass/test_stdlib_random.nari` + `test_stdlib_uuid.nari`.)_
- [x] **Networking breadth** - TCP/UDP client surface (today `net` is essentially server-create + basic `http`). _(Done - `net.connect(host, port)` returns a handle resolving to `{fd, ip, port}`; `net.listen(port)` + `net.accept(server)` for non-blocking TCP servers (port=0 -> ephemeral); `net.read/write/close(conn)` + `net.closeServer(server)` for IO; `net.udpSocket(port)`/`net.udpSend(sock, host, port, data)`/`net.udpRecv(sock, timeoutMs)`/`net.udpClose(sock)` for datagram surface; loopback client+server covered by `tests/expect_pass/test_stdlib_net.nari` under release + sanitize.)_
- [x] **Publish a generated stdlib API reference.** _(Done - `tools/gen_stdlib_reference.py` parses `src/stdlib/std/*.nari` (prelude + 11 modules), extracts module preamble + per-field `// ...` docstrings, and emits `docs/stdlib-reference.md` with one section per namespace and signatures for each member; covers 26 namespaces / 131 members. Regenerate with `tools/gen_stdlib_reference.py docs/stdlib-reference.md src/stdlib/std`. Linked from `docs/README.md`.)_

## P1 - Package manager (npkg)

Already implemented: `init`, `install`, `add`, `clean`, `list`, `update`, `publish`, `login`, `search`, `unpublish`, `owner` (`ls`/`add`/`rm`). Solid base - gaps are runner UX and specs.

- [ ] **`npkg run` / `npkg exec`** (script runner / dependency-bin execution) - not present.
- [ ] **Publish a manifest spec** and a **lockfile format spec** (modules exist: `manifest`, `lockfile`, `lockfile_reader`, `semver`, `store`, `workspace`).
- [ ] **Workspace/monorepo polish** + documented resolution rules.
- [ ] **Credential storage spec** for private registries (auth token handling/rotation).
- [ ] **End-to-end npkg integration tests** in CI (publish->install->update->unpublish round-trip).

## P1 - Registry server

Already implemented: per-IP rate limiting (`authLimiter`/`uploadLimiter`), body-size caps (1 MiB / 64 MiB), transactional publish (`Begin`/`Commit`/`Rollback`), path-traversal defense on version-derived paths, GitHub OAuth + device flow, owners API. (Roadmap marking these undone is stale.)

- [ ] **Yanking / soft-delete semantics** (distinct from hard `unpublish`) with index propagation.
- [ ] **DB migration system** (versioned, repeatable) instead of ad-hoc schema.
- [ ] **Email verification** for accounts.
- [ ] **Registry integrity checker** (archives <-> DB <-> checksums reconciliation job).
- [ ] **Search ranking/indexing** beyond basic lookup.
- [ ] **Takedown / abuse policy** + tooling.
- [ ] **Hosted index deployment** (a real, reachable registry) + ops runbook.
- [ ] **Server test suite** (handlers/auth/middleware) wired into CI.

## P1 - CI / testing / QA

Today CI is three workflows (`build-linux`, `build-windows`, `release`). Linux builds and runs `./run_tests.sh` in **one** mode only. Big coverage gaps:

- [ ] **Run the test suite in BOTH tree-walk and bytecode modes** (and exercise `--tree-walk`).
- [x] **Sanitizer jobs** - ASan + UBSan (directly guards the P0 UB/UAF items). Done via `.github/workflows/sanitize.yml` + `run_tests.sh --sanitize`; see the P0 entry above.
- [ ] **Fuzzing** - parser, bytecode verifier, JSON, and LSP inputs.
- [ ] **Frontend CI** - build + typecheck + lint for the Vite/Solid app (currently none).
- [ ] **Server CI** - Go build/test/vet for the registry.
- [ ] **npkg integration tests** (see package-manager section).
- [ ] **GC stress tests** + **benchmark suite with perf-regression gating.**
- [ ] **Confirm Windows actually runs tests** (not just builds) and add it to the release matrix.
- [ ] **Conformance test suite** tied to the language spec once it exists.

## P1 - Documentation & governance

- [ ] **Formal language spec** (also listed above - it's both a docs and a core item).
- [ ] **`CONTRIBUTING.md`** - missing.
- [ ] **`SECURITY.md`** (vuln disclosure policy) - missing.
- [ ] **`CODE_OF_CONDUCT.md`** - missing.
- [ ] **`CHANGELOG.md` + versioning/release policy** - missing; define SemVer commitment.
- [ ] **Issue/PR templates** (`.github/ISSUE_TEMPLATE`, `PULL_REQUEST_TEMPLATE`) - missing.
- [ ] **Stdlib API reference + manifest/lockfile specs** (cross-listed).
- [ ] **Security/threat model doc** for the runtime + registry.

---

## P2 - Tooling: LSP, formatter, editors

- [ ] **Modularize the LSP** - `lsp/lsp_server.cpp` is a single 3,312-line file; split by feature.
- [ ] **`nari fmt` - there is no formatter anywhere.** Ship one (and wire it into LSP "format on save" + CI fmt-check).
- [ ] **LSP feature breadth** - rename, find-references, code actions, inlay hints, semantic tokens, cross-file analysis, incremental reparse.
- [ ] **Configurable LSP logging** (don't log to `/tmp` by default) + LSP integration tests.
- [ ] **Publish editor extensions** - VS Code Marketplace + Zed (a VS Code extension source exists; needs publishing).

## P2 - Debugger (DAP)

- [ ] **Implement evaluate/watch + exception breakpoints + richer data inspection** (`src/dap/dap_server.cpp:1069` is literally "Accepted but not implemented").
- [ ] **DAP integration tests.**

## P2 - Compiler / bytecode / VM

- [ ] **Centralize opcode metadata** (finish the table-driven cleanup the TODOs reference).
- [ ] **Deepen the bytecode verifier** semantics (pairs with the P0 "gate JIT on verifier" item).
- [ ] **Bytecode-mode test coverage** in CI (cross-listed).
- [ ] **Float comparisons feeding a branch don't trace-JIT accelerate (CONFIRMED, perf - possibly a masked codegen bug).** A hot loop with a float `==`/`!=`/`<`/`<=`/`>`/`>=` driving an `if`/`while` runs at interpreter speed (correct output via deopt fallback, but **no speedup**), whereas float *arithmetic* (~180x) and *int* comparisons (~270x) both accelerate fully. Affects the entire relational family (`FloatLt/Le/Gt/Ge`) and the new `FloatEq/FloatNe`. Root cause not yet isolated - either the float-compare guard's trace silently fails to compile (hits the asmjit `default` -> `compilation_ok=false`) or it deopts/blacklists on first entry. The deopt safety net keeps output correct, which *masks* whatever the underlying codegen issue is. Measurement harness: function-wrapped hot loop, compare `NARI_DISABLE_TRACE_JIT=1` vs default. - `src/jit/trace_jit_asmjit.cpp` (`make_lazy_float`, `CondExitIfFalse`/`SideExit` consumers).

## P2 - FFI

`src/ffi.cpp` (libffi-based) works but is x86-centric and has placeholders.

- [ ] **ARM64 support** (calling-convention coverage).
- [ ] **Nested structs, struct arrays, struct-returning callbacks.**
- [ ] **Result-based error reporting** instead of throws/placeholders; remove `FFI::` stubs.
- [ ] **A documented FFI safety/capability policy** (it's inherently unsafe - gate it).

## P2 - Runtime safety & capabilities

- [ ] **Capability/sandbox model** - gate filesystem/network/process/`Spawn` and GC-control builtins behind explicit permissions.
- [ ] **Document the unsafe builtin surface** and distinguish "empty file" vs "missing file" semantics in `io`/`fs`.
- [ ] **Lifecycle/RAII review** for native handles (files, sockets, FFI) to guarantee release.

## P2 - Web frontend

Vite + Solid/TS; pages exist (Home, Search, PackageDetail, Dashboard, Login, Register, AuthCallback, CLILogin).

- [ ] **Gitignore the build output** - `npkg-frontend/web/dist` is currently untracked *but not ignored* (no matching `.gitignore` rule), so it's one `git add .` away from being committed. Add an ignore rule.
- [ ] **README rendering** on package pages.
- [ ] **Version history + yanked-version UI.**
- [ ] **Richer package metadata pages** (deps, owners, downloads).
- [ ] **Loading/error-state polish** + accessibility pass.

## P2 - Release, distribution & CLI ergonomics

Already in place: tag-driven `release.yml` for linux x64/arm64 + macOS x64/arm64, tarballs with per-file `.sha256` + `SHASUMS256.txt`, GitHub Releases, Homebrew formula, AUR `-bin`, `install.sh`/`uninstall.sh`, update scripts.

- [ ] **`--version` flag** - none of the binaries support it (only `--help`). Add it everywhere.
- [ ] **Unified `nari` CLI locally** - binaries are `interpreter`/`naric` and only renamed to `nari` in the release tarball; provide `nari run`/`nari build`/`nari fmt`/`nari test` UX.
- [ ] **Signed artifacts** - currently only SHA-256 sums; add GPG or cosign signatures + verification docs.
- [ ] **Windows in the release matrix** + **Scoop/winget** manifests.
- [ ] **Official Docker image.**
- [ ] **WASM build + browser playground.**

## P2 - Build & infra hygiene

- [ ] **Remove the Python build-time dependency** for resource embedding (`tools/embed_resource.py`) or document/guard it.
- [ ] **Harden all build scripts** consistently (`set -euo pipefail` everywhere, like `run_tests.sh`).
- [ ] **REPL/stdlib load caching** for faster startup.

---

## P3 - Ecosystem reach (nice-to-have)

- [ ] Project scaffolding templates (`npkg init` presets / starters).
- [ ] Curated example gallery + cookbook.
- [ ] Documentation generator for Nari packages (doc comments -> site).
- [ ] Package quality/health signals on the registry (downloads, freshness, lint score).
- [ ] Playground sharing / embeddable snippets (depends on WASM build).

---

## Snapshot: what's already solid (so the list isn't all doom)

- Mature **tag-driven release pipeline** (multi-arch Linux+macOS, checksums, Homebrew/AUR, installers).
- **Registry server** already has rate limiting, body caps, transactional publish, path-traversal defense, OAuth + device-flow login, owners API.
- **npkg** already covers the core verbs incl. `publish`/`unpublish`/`search`/`owner`.
- A **self-hosted JIT** (method + trace) with an honest, written **UB audit** (`JIT_UB_AUDIT.md`) - rare and valuable; the P0 list above is mostly executing on that audit.
- `Result`/`Option` types, a working LSP + DAP, REPL, and 19 tutorial docs.

> **Highest-leverage next moves:** (1) clear the P0 list, (2) write the language spec, (3) split + grow the stdlib with a stability policy, (4) add tree-walk/bytecode + sanitizer CI, (5) add the missing governance files. Those five unlock most of "proper ecosystem."
