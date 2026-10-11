<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The libzxv C ABI

Every ZXV SDK is a thin layer over one C library, `libzxv`, declared in
[`sdk/libzxv/include/zxv_api.h`](../libzxv/include/zxv_api.h). This page
explains the rules in that header with small examples. If you write a new
binding, read this page and [SDK_DESIGN.md](SDK_DESIGN.md); if you only use
an SDK, you can skip it.

ABI version: **1.1.0**. Check it at load time with `zxv_version`.

## Building it

```sh
make -C sdk/libzxv            # build/libzxv.so (or .dylib) and build/libzxv.a
make -C sdk/libzxv test       # 916 C conformance checks
make -C sdk/libzxv SANITIZE=1 test   # the same under ASan + UBSan
make -C sdk/libzxv fixtures   # sdk/conformance/fixtures: tiny test model
make -C sdk/libzxv dist       # build/dist/{include,lib} for packaging
```

Options: `ZXV_NO_PQC=1` (no ML-KEM, ML-DSA, Ed25519), `ZXV_NO_TENSOR=1` (no
inference engine), `ZXV_NO_CBANK=1` (no ISO 4217 table). Calls into a
left-out group return `ZXV_E_NOTIMPL`, and `zxv_features` says so.

## The rules

### R1 Opaque handles

You only ever hold pointers to incomplete types (`zxv_ctx`, `zxv_cap`,
`zxv_model`, ...). No struct layout crosses the boundary, so bindings never
break when the kernel changes a struct.

### R2 Scalars only

Parameters are fixed-width integers, `size_t`, UTF-8 strings, byte buffers,
or out pointers to those. Money is exact `int64` minor units.

### R3 Status codes

Almost every function returns `int32_t`: `ZXV_OK` (0) or a negative
`ZXV_E_*`. Out parameters are written only on success.

```c
int64_t equity;
int32_t rc = zxv_ledger_balance_cap(cap, account, NULL, NULL, &equity);
if (rc != ZXV_OK) {
    char why[256]; size_t n;
    zxv_last_error(why, sizeof why, &n);
    fprintf(stderr, "%s: %s\n", zxv_status_name(rc), why);
}
```

### R4 Two-call buffers

Functions that return text or bytes take `(buf, cap, *out_len)`. Call once
with `NULL` to learn the size, then again with a buffer one byte larger.

```c
size_t need;
zxv_rat_format(1, 3, NULL, 0, &need);        /* ZXV_E_BUFFER, need = 3 */
char *s = malloc(need + 1);
zxv_rat_format(1, 3, s, need + 1, &need);    /* "1/3" */
```

### R5 UTF-8

Strings in and out are UTF-8. Invalid input is `ZXV_E_UTF8`.

### R6 Threading

A `zxv_ctx` is guarded by a mutex: share it across threads freely. Other
handles are documented per group. Destroy each handle once, after every
call on it has returned, and destroy children before parents.

### R7 Last error

`zxv_last_error` gives thread-local diagnostic text. Branch on the status
code, never on the text.

### R8 Versioning

The major version changes only on a breaking change. New functions, enum
values and status codes are minor. Treat an unknown negative status as an
error of class SYSTEM.

### R9 Features

`zxv_features()` returns a bitmask of `ZXV_FEAT_*` groups live in this
build. `ZXV_FEAT_LORA` is never set yet.

### R10 Capabilities (new in 1.1)

Every call that touches state, keys or the engine takes a `const zxv_cap *`
first and checks it on every call: the handle and all its ancestors are not
revoked, the needed bit is set, and the attestation is `ZXV_LP_TRUE`.

```c
zxv_cap *root, *reader;
zxv_cap_create(ctx, 1, NULL, ZXV_CAP_ALL, &root);
zxv_cap_attenuate(root, ZXV_CAP_LEDGER_READ, &reader);  /* narrow only */
zxv_ledger_post_cap(reader, a, b, 100, "NGN", "ref-1", &id);  /* ZXV_E_CAP */
zxv_cap_revoke(root);               /* reader is now dead too */
```

| Bit | Allows |
|---|---|
| `LEDGER_READ` | balances, journals |
| `LEDGER_POST` | postings |
| `CRYPTO_SIGN` | ML-DSA-87, Ed25519 signing |
| `CRYPTO_KEM` | ML-KEM-1024 |
| `AI_INFERENCE` | model open, generate |
| `BUDGET` | rmag budgets |
| `PHASE` | phase clock admit, defer, advance |
| `ATTEST` | lpres attestations |
| `SWARM` | swarm token economy |
| `EVM` | state-channel encoding |

Pure functions (rationals, logic tables, hashes, wire framing) take no
capability.

Honest limit: inside one process this is least-privilege discipline, not a
sandbox. Code in the same address space can create its own root
capability. It becomes enforcement at a process boundary (the `zxvd`
daemon over IPC).

### R11 No floats (new in 1.1)

No parameter is `float` or `double`. Exact values cross as two `int64`
(numerator, denominator). Engine parameters are Q16.16 in an `int32`
(value = q / 65536). A host double enters only through
`zxv_rat_from_ieee754(bits)`, which takes the raw IEEE-754 bits and returns
the exact rational they denote: 0.1 becomes
3602879701896397/36028797018963968. NaN and infinity are `ZXV_E_FLOAT`.

## Status classes

`zxv_status_class(rc)` sorts every code into one of five classes, and every
SDK maps them to the same exception families.

| Class | Codes (examples) | What to do |
|---|---|---|
| OK (0) | `ZXV_OK` | |
| USAGE (1) | `ARG`, `RANGE`, `UTF8`, `FORMAT`, `FLOAT`, `RMAG_INVALID` | fix the call |
| REFUSED (2) | `CAP`, `FUNDS`, `BUDGET`, `VERIFY`, `NOTIMPL`, `STATE`, `DECLINED` | handle it; may succeed later |
| FATAL (3) | `INVARIANT`, `POISONED` | stop; never retry |
| SYSTEM (4) | `NOMEM`, `IO`, `INTERNAL`, unknown | environment |

## Fail-closed

When the ledger finds a broken invariant it rolls back and **poisons** the
context: every later call on it returns `ZXV_E_POISONED`, whatever the host
does. `zxv_ctx_is_poisoned` reports it and `zxv_ctx_halt(ctx, reason)`
triggers it on purpose (for tests, or when the host detects something
wrong). A C library cannot make a host exception uncatchable; poisoning is
what makes retrying impossible. SDKs then raise a terminal error type and
offer an opt-in policy that aborts the process.

## Groups added in 1.1

| Group | Prefix | Kernel module |
|---|---|---|
| Exact rationals | `zxv_rat_` | `kernel/src/rational` |
| Four-valued logic | `zxv_lp_`, `zxv_lpres_` | `kernel/src/lpres` |
| Capabilities | `zxv_cap_` | `kernel/src/sdk_bridge` model |
| Resource budgets | `zxv_rmag_` | `kernel/src/rmag` |
| Phase clock | `zxv_phase_` | `kernel/src/phase_coord` |
| UBH-168 frames | `zxv_wire_` | `kernel/src/harmonic` |
| Swarm economy | `zxv_swarm_` | `kernel/src/swarm` |
| Hashes | `zxv_sha3_256`, `zxv_shake256`, `zxv_keccak256` | `kernel/src/mlkem/keccak` + own Keccak-256 |
| Post-quantum crypto | `zxv_mlkem1024_`, `zxv_mldsa87_` | `kernel/src/pqsec` |
| Ed25519 | `zxv_ed25519_` | `kernel/third_party/ed25519` |
| Inference engine | `zxv_model_`, `zxv_session_`, `zxv_generate` | `kernel/src/tensor` |
| EVM state channels | `zxv_evm_` | own ABI encoder |

The header documents every function. The answer key every SDK tests
against is [`sdk/conformance/vectors.json`](../conformance/vectors.json),
regenerated by `make -C sdk/libzxv vectors`.
