<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ZXV kits

A **language SDK** (in [`sdk/`](../)) wraps `libzxv` for one programming
language. A **kit** is a ready-made building block for a kind of
application, written on top of the same C ABI
([`zxv_api.h`](../libzxv/include/zxv_api.h)) and held to the same rules:
opaque handles, `int32_t` status codes, exact integers and rationals, and
no floats.

| Kit | Folder | What it is | Status | Tests |
|---|---|---|---|---|
| Z-Wasm | [`z-wasm`](z-wasm/) | Plugin sandbox. A no-float load-time validator for WebAssembly plugins, fuel metering charged to an `rmag` budget, a wasm3 host with capability-gated host functions, and the S0 proposal queue (Tri-Isolate starter). | kit API 0.1 (preview) | 1140 validator checks, 126 host checks, mutation fuzzing under ASan + UBSan, a V8 cross-check |

## Rules every kit follows

- **Only the public ABI.** A kit calls `zxv_api.h` and nothing under
  `kernel/`. A kit never needs a change to `libzxv` to build.
- **No floats** in kit interfaces, and none in kit code that decides
  anything (rule R11 in [ABI.md](../docs/ABI.md)).
- **Honest limits.** Each kit README has a threat model or limits section
  that says what the kit does not guarantee.
- **Versioning.** A kit has its own version. Until a kit reaches 1.0, its
  API may change. The libzxv ABI under it does not
  ([STABILITY.md](../docs/STABILITY.md)).
- **Third-party code** is vendored unmodified, with its licence and
  checksums recorded in the kit's `THIRD_PARTY.md`, and only under licences
  compatible with Apache-2.0.
- **CI.** Each kit has jobs in
  [`.github/workflows/sdk.yml`](../../.github/workflows/sdk.yml) on Linux and
  macOS, plus a sanitizer run.
