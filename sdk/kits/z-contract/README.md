<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# z-contract: typed contracts with exact bounds

z-contract is a small interface definition language for the values that
cross a plugin boundary. A contract says which values a function may take
and give back: integer and exact-rational ranges, denominator limits,
four-valued truth sets, 64-state tag sets and byte lengths. The kit parses a
contract, checks values against it exactly (never through a float), and
keeps a hash-chained evidence log of every violation.

```text
# fees.zc
fn fee(amount: int[0..10^12], bps: int[0..10000]) -> int[0..0]
    proposes rat[0..10^12] den|100 max 1;
```

The kit has three parts:

1. **Parse.** A hand-written recursive-descent parser. A refusal names the
   line, the column, the byte offset and what was expected, for example
   `line 6, column 31: expected '<=' or '|' after den, found '<'`.
2. **Check.** `zc_check` compares values with a function's parameters,
   result or proposals. Rationals are compared with `zxv_rat_cmp` (exact,
   128-bit cross-multiplication in libzxv). A violation names the part, the
   index, a `ZC_V_*` reason and a message.
3. **Enforce on Z-Wasm.** `zc_wasm_attach` puts a contract on a
   [Z-Wasm](../z-wasm/) plugin export. Bad arguments are refused before the
   plugin runs. A bad result or proposal stops the plugin exactly like a
   trap: its proposals are dropped before they reach the S0 queue, and it
   becomes DEAD. Every violation is appended to an evidence log whose
   records are chained with SHA3-256 (`zxv_sha3_256`).

> **What a contract is, and is not.** It checks the *shape and bounds* of
> values. It cannot tell whether a value inside the bounds is the right
> value: a fee plugin that charges 599 instead of 600 passes a contract that
> allows 0..10^12. See [Threat model](#threat-model).

Kit API version 0.1 (`include/zcontract.h`, `include/zcontract_wasm.h`).
It may still change before 1.0. It calls only the libzxv ABI 1.2
(`sdk/libzxv/include/zxv_api.h`) and, for the Wasm binding, the Z-Wasm kit
API 0.2.

## Quickstart

```sh
make -C sdk/libzxv                         # the C library every SDK wraps
python3 -m pip install ziglang             # only for the example plugin
make -C sdk/kits/z-contract test demo
sdk/kits/z-contract/build/zc-tool check sdk/kits/z-contract/examples/fees.zc
```

The demo enforces [`examples/fees.zc`](examples/fees.zc) on Z-Wasm's
rational example plugin:

```text
fee(48000, 125): ZXV_OK
fee(-5, 125): ZW_E_CHECK
  refused before running: BELOW, evidence #1: fee: parameter 0 'amount' = -5 is below the minimum 0
fee(333, 125): ZW_E_CHECK
  plugin stopped: DENOMINATOR, evidence #2: fee: proposal 0 = 4.1625 (333/80): denominator 80 does not divide 100
fee(48000, 125): ZW_E_DEAD
proposals waiting for the host: 1
evidence #1 [false/neutral/potential] eb021e57...: fee: parameter 0 'amount' = -5 is below the minimum 0
evidence #2 [false/forbidden/manifest] 344c6a00...: fee: proposal 0 = 4.1625 (333/80): denominator 80 does not divide 100
evidence chain: verified
```

`zc-tool check` prints the canonical form of a contract, or the error in
compiler style:

```text
examples/broken.zc:6:31: expected '<=' or '|' after den, found '<'
      proposes rat[0..10^12] den<100 max 1;
                                ^
```

### In a host

```c
#include "zcontract_wasm.h"

zc_contract *c;  zc_evlog *log;  zc_wasm_guard *g;
uint32_t line, col;  char msg[256];
if (zc_parse(text, len, &c, &line, &col, NULL, msg, sizeof msg, NULL) != ZXV_OK)
    /* msg: "line 3, column 12: empty range: ..." */;
zc_evlog_create(4096, &log);
zc_wasm_attach(plugin, "fee", c, "fee", log, &g);     /* every call is checked now */
int32_t st = zw_plugin_call(plugin, "fee", args, 2, &res, NULL, NULL);
if (st == ZW_E_CHECK)
    zc_wasm_last_violation(g, &part, &index, &reason, &seq, msg, sizeof msg, NULL);
/* teardown, children first */
zc_wasm_detach(g);  zw_plugin_destroy(plugin);  zc_evlog_destroy(log);  zc_contract_destroy(c);
```

The checker works without Wasm too. Build values with `zc_vals_push_*` and
call `zc_check(c, "fee", ZC_PART_PARAMS, vals, ...)`. The
[adapter-matrix](../adapter-matrix/) kit does this for external records.

## Syntax

```text
contract  = { fn_decl } EOF
fn_decl   = "fn" ident "(" [ param { "," param } ] ")"
            [ "->" type ] [ "proposes" type [ "max" uint ] ] ";"
param     = name ":" type
name      = ident { "." ident }                   no spaces: order.price
type      = "int"   [ "[" ibound ".." ibound "]" ]
          | "rat"   [ "[" rbound ".." rbound "]" ] [ "den" ( "<=" | "|" ) uint ]
          | "truth" [ "{" truthname { "," truthname } "}" ]
          | "tag"   [ "{" tagpat { "," tagpat } "}" ]
          | "bytes" [ "[" uint ".." ( uint | "*" ) "]" ]
ibound    = "*" | int                 int = [ "-" ] uint [ "^" uint ]
rbound    = "*" | rat                 rat = int | [ "-" ] uint "." digits | [ "-" ] uint "/" uint
truthname = "neither" | "true" | "false" | "both"
tagpat    = part "/" part "/" part     base/polarity/phase; a part is a name or "*"
comment   = "#" to the end of the line
```

| Type | Values | Bounds |
|---|---|---|
| `int[lo..hi]` | `int64`, or a rational that reduces to an integer | inclusive, `*` for an open end, `10^12` and `-2^40` notation |
| `rat[lo..hi] den<=N` / `den\|N` | exact rationals (two `int64`) | inclusive; bounds can be `-1.5`, `3/4`, `10^9`. `den<=N` limits the **reduced** denominator; `den\|N` requires it to divide N (`den\|100` means "whole cents") |
| `truth{...}` | the four-valued `ZXV_LP_*` | a set of names; no set means all four |
| `tag{...}` | the 64-state tag (see below) | patterns such as `true/*/manifest` or `*/forbidden/*` |
| `bytes[lo..hi]` | byte strings | the length, inclusive |

Rules the parser enforces, each with a position: ranges must not be empty
(`[5..3]`), every number must fit `int64` exactly (`2^63`, the decimal
`0.12345678901234567890` and `-2^63` are refused, because libzxv refuses
`INT64_MIN`), a denominator is never zero, names are at most 63 bytes,
function and parameter names are unique, set entries are not repeated,
`proposes` takes `int` or `rat` (proposals are exact rationals), and
`max` is 1..64. Only ASCII is allowed outside comments, and the whole text
must be valid UTF-8. A function with no `->` returns nothing. A function
with no `proposes` must propose nothing.

**Canonical form.** `zc_contract_format` writes one declaration per line,
with numbers written out (`10^12` becomes `1000000000000`, `6/4` becomes
`1.5`, `int[*..*]` becomes `int`) and no comments. Parsing the canonical
form gives the same canonical form (the fuzzer checks this on every
accepted input). `zc_contract_digest` is the SHA3-256 of that text, so
whitespace and comments do not change a contract's identity, but any bound
does.

## Checking

`zc_check(contract, fn, part, vals, &reason, &index, msg, cap, &len)`:

| Reason | Meaning |
|---|---|
| `ARITY` | wrong number of parameters, or a result that is missing or not declared |
| `KIND` | a value of another kind (a byte string for an `int`) |
| `INVALID` | not a value: a zero denominator, `INT64_MIN`, a truth above 3, a tag above 63 |
| `NOT_INTEGER` | a non-integral rational for an `int` |
| `BELOW`, `ABOVE` | outside the range |
| `DENOMINATOR` | the reduced denominator breaks `den<=N` or `den\|N` |
| `NOT_IN_SET` | a truth or tag value outside the declared set |
| `LENGTH` | a byte length outside `bytes[lo..hi]` |
| `COUNT` | more proposals than `max N`, or any proposal when none are declared |

Values are reduced before they are judged, so `6/12` is `1/2`. An `int`
slot takes a rational with denominator 1, and a `rat` slot takes an `int`.
Exactness is the point. The double nearest 0.1 is
3602879701896397/36028797018963968 (from `zxv_rat_from_ieee754`), and
`rat[0..1/10]` refuses it as `ABOVE`. The tests check this. Any comparison
that cannot be made is a violation, never a pass.

Messages show a terminating decimal and the reduced fraction it stands for,
`4.1625 (333/80)`, because the fraction's denominator is what `den` limits.
Branch on the reason code, not on the text.

## Z-Wasm binding

`zc_wasm_attach(plugin, export, contract, fn, log, &guard)` uses the Z-Wasm
call-check hook (`zw_plugin_set_check`, Z-Wasm kit API 0.2). Every
`zw_plugin_call` of that export is checked, whoever makes the call:

| Stage | Checked | On a violation | Evidence tag |
|---|---|---|---|
| before the call | the arguments | `ZW_E_CHECK`. Nothing runs, no fuel is used, and the plugin stays READY, because bad input is the caller's fault. | `false/neutral/potential` (an ordinary refusal of work not started) |
| after a normal return | the result, then every staged proposal | `ZW_E_CHECK`. The plugin is DEAD and its proposals never reach the S0 queue, as after a trap. The call's fuel is still charged. | `false/forbidden/manifest` (the kernel's action for a forbidden tag is rollback and quarantine) |

How types map onto Wasm arguments: `int`, `truth` and `tag` take one
`i32`/`i64` argument, and `rat` takes two (numerator, then denominator).
`bytes` cannot be passed (`ZC_E_MAPPING` at attach). The single Wasm result
can be `int`, `truth` or `tag`. A rational result goes out through
`proposes rat ...`, and attach refuses `-> rat`. At most 8 flattened
arguments are allowed. `zc_wasm_args` flattens typed values for a caller.

**Fail closed.** If the evidence log refuses a record, because it is full
or its sink refused, the call is refused, and the guard then refuses every
later call (`zc_wasm_stats` reports `log_failed`). A violation is never
left unrecorded.

## Evidence log

`zc_evlog` is append-only. Record *k* holds a sequence number, the origin
(host, Wasm or adapter), the part, the index, the reason, the tag, the
contract digest, a *subject* digest (`zc_vals_digest`: SHA3-256 of the
offending values, so the log identifies the data without copying it) and a
text of at most 255 bytes, cut at a UTF-8 boundary. It is chained:

```text
hash_k = SHA3-256("zxv/zc-evlog/v1" || seq u64le || hash_{k-1} || origin u32le
                  || part i32le || index u32le || reason i32le || tag i32le
                  || contract[32] || subject[32] || text_len u32le || text)
```

`zc_evlog_verify` recomputes every link and finds an altered field, an
altered text, reordered records or a dropped last record. A full log
refuses the append (`ZXV_E_CAPACITY`) instead of evicting old records.
Signing the head (`zc_evlog_head`) with ML-DSA-87 or Ed25519 through libzxv
is the host's job.

### Switching to the 64-state quarantine log (ABI 1.3)

libzxv ABI 1.3 will publish the kernel's 64-state tag and its quarantine
log as `zxv_hex_*` functions. z-contract is built so it can switch over
without an API change:

- `zc_evlog_set_sink(log, fn, user)` calls `fn` for every record after it is
  hashed and before it is committed, with the tag, the origin (the
  quarantine log's *submitter*), the reason, the subject digest and the
  hash. An ABI 1.3 host passes a sink that appends the same record to the
  libzxv quarantine log. If the sink refuses, the record is refused
  (`ZC_E_LOG`) and the Wasm guard fails closed.
- Output violations already carry a forbidden tag, the only kind that the
  kernel's log quarantines.

## The 64-state tag (provisional)

Until ABI 1.3 ships `zxv_hex_*`, `zcontract.h` carries a placeholder with
the kernel's layout ([`lpres_hex.h`](../../../kernel/src/lpres/lpres_hex.h)):
`code = base << 4 | polarity << 2 | phase`, with base `false 0`,
`neither 1`, `both 2`, `true 3` (not the `ZXV_LP_*` numbering), polarity
`entropic`, `neutral`, `forbidden`, `negentropic`, and phase `potential`,
`emerging`, `dissolving`, `manifest`. `ZC_TAG_PROVISIONAL` is 1.

The **text** form `base/polarity/phase` is the stable interface. Contracts,
logs and adapters use names. `zc_tag_make`, `zc_tag_split`,
`zc_tag_parse` and `zc_tag_format` will be re-implemented on `zxv_hex_*`
with the same signatures. Do not persist the numeric code.

## Threat model

**Assets.** The host's ledger and queue (what reaches them), and an honest
record of what was refused.

**Adversaries.** A plugin author who returns or proposes whatever they
like. A caller that passes out-of-range input. A contract author who
writes a contract the parser should refuse.

| Protection | Mechanism |
|---|---|
| Out-of-bounds outputs never reach the queue | the after-call check runs before staged proposals are queued; a violation drops them and kills the plugin |
| No bypass inside the host's API | the check is attached to the plugin's export, so every `zw_plugin_call` of it is checked, not only calls made through a wrapper |
| Exact bounds | integer and rational comparisons only (R11: no float type in the kit, checked by `check-nofloat`); comparisons that cannot be made fail |
| Parser robustness | bounded input (1 MiB, 256 functions, 16 parameters, 64-entry sets), no recursion, fuzzed under ASan and UBSan |
| Tamper-evident evidence | SHA3-256 hash chain, full-log refusal, fail-closed guard |

**Out of scope, and the honest limits:**

- **Correctness inside the bounds.** A contract cannot tell a right value
  from a wrong one that is in range. The host still judges every proposal.
- **In-process enforcement.** The checker runs in the host's process. Host
  code can detach a guard, write to the log's memory or call the plugin with
  checks removed. That is least privilege, not a sandbox, the same honest
  limit libzxv states for capabilities.
- **The log is evidence, not a seal.** The hash chain shows that records
  were changed, reordered or truncated, but an attacker with write access
  can rebuild the whole chain. Sign the head and store it elsewhere to
  close that gap.
- **i32 parameters.** Z-Wasm passes the low 32 bits of an argument to an
  `i32` parameter. The guard checks the `int64` the caller passed. For an
  export with `i32` parameters, keep the contract's range inside
  `int32` (the guard cannot see the export's parameter types through the
  Z-Wasm API).
- **One guard per export.** Attaching to an export that already has a
  check replaces it, and `zc_wasm_detach` removes whatever check that
  export has.
- **Threads.** A guard, a log and a `zc_vals` are not thread-safe. A parsed
  contract is read-only after `zc_parse` and can be shared.
- **The 64-state tag is provisional** until ABI 1.3. Use the text names.
- **Kit API 0.x.** It is not yet covered by the libzxv stability promise.

## Limits

| Limit | Value |
|---|---|
| contract text | 1 MiB (`ZC_MAX_TEXT`) |
| functions | 256 |
| parameters per function | 16 |
| name | 63 bytes |
| entries in a set | 64 |
| values in a `zc_vals` | 64 |
| one byte string | 1 MiB |
| `max` proposals | 64 (Z-Wasm stages at most 64 per call) |
| evidence log | 1 .. 2^20 records, text 255 bytes |
| Wasm arguments after flattening | 8 |

## Tests

```sh
make test                          # everything below except the long runs
make SANITIZE=1 OUT=build/san test # the same under ASan + UBSan (z-wasm rebuilt too)
make fuzz FUZZ_ITERS=500000        # deterministic mutation fuzzer
ZC_FUZZ_SEED=3 make fuzz           # another deterministic stream
make fuzz-libfuzzer                # libFuzzer, if clang ships its runtime
make demo                          # the demo above (needs zig)
```

| Suite | What |
|---|---|
| `test_parse` | 473 checks: 19 accepted texts against their exact canonical form, round trips and digests; 45 refusals, each with its exact line, column and message, the offset agreeing with them, and the last-error text; message truncation, `NULL` handling, non-terminated input, and every size limit (63/64-byte names, 16/17 parameters, 256/257 functions, 64/65 set entries, 1 MiB) |
| `test_check` | 101 checks: values, every `ZC_V_*` reason with its index and message, inclusive bounds, reduction, the 0.1 double refused by `rat[0..1/10]`, values one unit either side of 1/10 at the `int64` limit, results and proposal counts, subject digests, all 64 tags through text and make/split, and the evidence log (the documented record hash recomputed independently, the sink, refusal when full, UTF-8 cut, and four kinds of tampering) |
| `test_wasm` | 81 checks: input refusals with no fuel used and the plugin READY, output violations that kill the plugin with no proposal queued, record tags and digests, undeclared proposals, arity and missing or unexpected results, mapping errors, rational arguments, fail-closed on a full log, detach, and the z-wasm rational example plugin under `examples/fees.zc` |
| `fuzz_parse` | for every input: deterministic status, position and message; the position inside the input and consistent with line and column; truncated messages are prefixes; accepted contracts round-trip to the same canonical text and digest; values at each declared `int` bound pass and one step outside fail at that parameter |
| `check-nofloat` | R11 over every kit source (z-wasm's checker) |

Clean runs on this branch: the three suites under gcc and clang, the same
under ASan + UBSan with leak detection, and 4 x 250,000 sanitized fuzz
mutants (seeds 1-4, about 38,000 accepted contracts each).

## Files

| Path | |
|---|---|
| `include/zcontract.h` | the kit API: parser, values, checker, tag placeholder, evidence log |
| `include/zcontract_wasm.h` | the Z-Wasm binding |
| `src/zc_parse.c`, `src/zc_check.c`, `src/zc_evlog.c`, `src/zc_util.c`, `src/zc_wasm.c` | the implementation (R11: integer code only) |
| `examples/fees.zc`, `examples/broken.zc` | contracts (the second must fail) |
| `examples/contract_demo.c` | the demo above |
| `tools/zc_tool.c` | `zc-tool check` / `zc-tool digest` |

## Licence

Apache-2.0. Registered to 36N9 Genetics LLC, Michael Laurence Curzi.
Provided without warranty.
