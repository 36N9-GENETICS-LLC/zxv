<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ZXV SDKs

Every ZXV SDK wraps one C library, [`libzxv`](libzxv/), so the same exact
arithmetic, four-valued logic, capability checks, post-quantum crypto and
integer inference engine are available in every language, and every SDK is
tested against the same answer key ([`conformance/vectors.json`](conformance/vectors.json)).

**New here?** Pick your language below, open its README, and run the
quickstart. Then read [Concepts](#concepts) once; the ideas are the same in
every language.

## Language SDKs

| Language | Folder | Package | How it binds | Tests |
|---|---|---|---|---|
| C / C ABI | [`libzxv`](libzxv/) | `libzxv.so` / `.dylib` | the library itself | 916 checks, ASan + UBSan |
| Java, Kotlin, Scala | [`java`](java/) | `org.zedec.zxv` | Java 22 FFM (no JNI) | 37 |
| .NET (C#, F#, Unity) | [`dotnet`](dotnet/) | `Zxv.Sdk` | `LibraryImport` | 125 |
| Python | [`python`](python/) | `zxv-sdk` | cffi, GIL released | 99, `mypy --strict` |
| TypeScript / JavaScript | [`typescript`](typescript/) | `@zedec/zxv` | Node-API addon and WebAssembly | 83 on each backend |
| Solidity / EVM | [`solidity`](solidity/) | `ZxvStateChannel.sol` | verifies libzxv's state encoding | 10 |
| Ruby | [`ruby`](ruby/) | gem `zxv` | ffi | 40 runs |
| PHP | [`php`](php/) | `zedec/zxv` | PHP FFI | 31 |
| Rust | [`rust`](rust/) | crates `zxv`, `zxv-sys` | bindgen, `Result` types | 40 |
| Go | [`go`](go/) | `.../sdk/go/zxv` | cgo, `error` returns | 36, `-race` |
| C++ | [`cpp`](cpp/) | header-only `zxv.hpp` (C++20) | direct C ABI, RAII | 409 checks, ASan + UBSan + TSan |
| Swift | [`swift`](swift/) | SwiftPM `Zxv` | C module map | 20 |

## Kits

Kits are building blocks on top of the same C ABI; see
[`kits/`](kits/). The first is [Z-Wasm](kits/z-wasm/), a sandbox for
WebAssembly plugins: a no-float validator, fuel charged to an `rmag`
budget, and a proposal queue the host must approve.

## What every SDK gives you

| Feature | Type names | Guide |
|---|---|---|
| Exact numbers, no floats | `Rational` | [SDK_DESIGN §3](docs/SDK_DESIGN.md#3-exactness-no-floats-at-the-boundary) |
| Four-valued logic | `Truth` (`NEITHER`, `TRUE`, `FALSE`, `BOTH`) | [§4](docs/SDK_DESIGN.md#4-four-valued-logic) |
| Errors and fail-closed | `ZxvException` family, `ZxvFatal…` | [§5](docs/SDK_DESIGN.md#5-errors) |
| Capabilities on every call | `Capability` | [§6](docs/SDK_DESIGN.md#6-capabilities-on-every-call) |
| Wiped secret keys | `SecretKey`, `MlKem1024`, `MlDsa87`, `Ed25519` | [§7](docs/SDK_DESIGN.md#7-secrets) |
| Budgets with async waits | `Budget` | [§8](docs/SDK_DESIGN.md#8-budgets-and-async) |
| Phase clock (27,720 ticks) | `PhaseClock` | [§9](docs/SDK_DESIGN.md#9-phase-ticks) |
| Integer inference engine | `Model`, `Session` | [§10](docs/SDK_DESIGN.md#10-engine) |
| UBH-168 wire frames | `Ubh168Frame` | [§11](docs/SDK_DESIGN.md#11-wire-framing-in-pure-host-code) |
| Swarm token economy | `Swarm` | [ABI](docs/ABI.md#groups-added-in-11) |
| EVM state channels | `StateChannel` | [solidity](solidity/) |

## Concepts

- [SDK design rules](docs/SDK_DESIGN.md): the names, error model and
  behaviour every SDK shares.
- [The C ABI](docs/ABI.md): the rules R1-R11 with examples, for binding
  authors.
- [ABI stability policy](docs/STABILITY.md): the never-break promise for
  binding maintainers, and the CI check that enforces it.
- [Security model and honest limits](docs/SECURITY.md): what is protected,
  and where protection stops.

## Building from source

```sh
make -C sdk/libzxv test fixtures   # the library, its tests, the tiny test model
```

Then follow the language README. CI builds and tests every SDK on Linux and
macOS ([`.github/workflows/sdk.yml`](../.github/workflows/sdk.yml)).

## Licence

Apache-2.0. Registered to 36N9 Genetics LLC, Michael Laurence Curzi.
Provided without warranty.
