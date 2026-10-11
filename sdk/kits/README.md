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
| Z-Wasm | [`z-wasm`](z-wasm/) | Plugin sandbox. A no-float load-time validator for WebAssembly plugins, fuel metering charged to an `rmag` budget, a wasm3 host with capability-gated host functions, call checks on exports, and the S0 proposal queue (Tri-Isolate starter). | kit API 0.2 (preview) | 1140 validator checks, 149 host checks, mutation fuzzing under ASan + UBSan, a V8 cross-check |
| z-contract | [`z-contract`](z-contract/) | Contracts for plugin boundaries. An IDL with exact-rational bounds, a parser with precise error positions, a checker, a Z-Wasm binding that stops a violating plugin like a trap, and a SHA3-256 hash-chained evidence log (ready to feed the ABI 1.3 quarantine log). | kit API 0.1 (preview) | 655 checks, parser fuzzing under ASan + UBSan |
| z-ffi | [`z-ffi`](z-ffi/) | A typed boundary for C libraries. A spec generates wrappers that need a capability, check integer ranges, take float arguments only when exact and return float results only as exact rationals; signatures are proved at compile time. A wrapper generator, not a sandbox. | kit API 0.1 (preview) | 369 checks over libc and libm, compile-time signature checks, spec fuzzing under ASan + UBSan |
| adapter-matrix | [`adapter-matrix`](adapter-matrix/) | Inbound data adapters: JSON (HTTP bodies), MQTT payloads and FIX tag=value. Exact decimals, a label on every record (accepted-unverified or refused with reasons), z-contract validation with evidence, host attestation behind `ZXV_CAP_ATTEST`. Labels data; cannot make a source accurate. | kit API 0.1 (preview) | 684 checks, adapter fuzzing under ASan + UBSan |
| AIPI | [`aipi`](aipi/) | Content-addressed functions. Runs functions named by CID (module, contract and checking rule hashed together) in Z-Wasm inside their z-contract, checks other nodes' answers with the function's own inverse or certificate, and makes certificates. The core ([`kernel/src/aipi`](../../kernel/src/aipi/aipi.h)) adds GET by hash, quorum, escrow and privacy. Compiles that kernel core in, an exception to the public-ABI rule until AIPI is in libzxv. Design: [docs/AIPI.md](../../docs/AIPI.md). | kit API 0.1 (preview) | 60 end-to-end checks on real Wasm (gcc, clang, ASan + UBSan); the core's 347 checks run in kernel `verify-all` |

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
