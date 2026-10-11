<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Hardware emulation and hardware updates

ZXV treats hardware the way it treats software: every component sits
behind an interface, may be replaced by anything that provably behaves the
same, and can be updated by a signed image. This page explains the design.
The code is in [`kernel/src/hwemu`](../../kernel/src/hwemu), and the C ABI
is libzxv 1.4 (`zxv_hw_*`).

## Three layers

```
 TARGET   the machine the software is written for
          (default: the public reference machine R1, a ZHP-1 profile)
    |  hwp_match: which target components does the host have?
    v
 MATRIX   one row per component function; each row has providers:
          FIXED silicon | RECONF region | PEER component | SOFT emulator
    |  the best proven provider serves each call
    v
 HOST     the machine that is physically here
          (a ZHP-1 profile from zxv_hw_profile_probe_host)
```

The machine is not emulated as one monolithic VM. Each component is its
own row in the matrix and is served independently, so a laptop can run the
CPU and vector work natively while the ternary unit, the hash unit and the
harmonic-wire port are emulated. When hardware that has those units
appears, its rows switch to native providers and nothing above changes.

### Profiles: the hardware ROM

[ZHP-1](HW_PROFILE.md) describes a machine as abstract component classes
with integer capability descriptors. It is canonical (one encoding, one
SHA-256 identity) and can carry per-component state, so a profile can be
signed, pinned, snapshotted and restored. It names classes and minimum
capabilities, never implementation details, so a vendor can publish a
profile for proprietary hardware without revealing anything about the
design. Private class and capability ranges are reserved for that.

### The matrix: components emulating each other

Every provider of a function has the same call shape: an opcode, input
bytes, output bytes. The rules (hwm.h M1-M7):

| Rule | What it guarantees |
|---|---|
| M1 one interface | any provider can replace any other |
| M2 proven before bound | a provider serves only after reproducing the function's conformance vectors bit for bit; mismatch is FALSE, no vectors is NEITHER (S0), a disagreement in service is BOTH, and only TRUE serves |
| M3 deterministic choice | lowest cost, then FIXED > RECONF > PEER > SOFT, then index: the same binding on every machine |
| M4 failover | a fault, a reconfiguration or a lost proof moves the function to the next proven provider; with none, the call is refused, never routed to an unproven one |
| M5 declared, acyclic dependencies | a provider may lean on other functions (an emulated hash using the matrix unit). It may call only what it declared, and no binding is circular, so A may emulate B and B may emulate A as cells without a live loop |
| M6 shadow checks | run the bound provider and the next-best one side by side; a mismatch takes both out of service until re-proven. Nothing votes |
| M7 memo | a pure function may skip the provider on a repeated input (exact byte compare) |

### Regions: hardware updates delivered as software

A reconfigurable region (FPGA fabric, a microcoded unit, a soft core)
changes what the hardware *is* when it loads a new image. hwr.h R1-R6:

1. **Signed.** `header || payload || ML-DSA-87 signature`. The signature
   covers the header, which carries the payload's SHA-256. Only keys the
   owner trusted verify, with the domain context `zxv-hwreconf-v1`.
2. **For this hardware only.** The header names a target class and an
   optional exact-part digest.
3. **Forward only.** Versions strictly increase, and a security version
   floor stops signed-but-vulnerable images from being replayed.
4. **Two slots.** The image loads into the inactive slot while the active
   one keeps working, then is read back and its digest compared.
5. **Proven on probation.** The trial switches slots and runs the
   function's conformance vectors through the matrix. While it switches,
   the matrix serves the function from an emulator, so callers do not
   stop. Only a passing image enters probation, and only a commit makes it
   permanent and raises the floor.
6. **Rollback and S0 quarantine.** Any failure after the signature checks
   out switches back to the previous image and re-proves it. If there is
   none, the region drops to S0 (unknown never acts) and the emulator keeps
   serving. The image goes into the hash-chained quarantine log (a
   forbidden hexagram tag) and is blocked from loading again. A full log
   halts the region and poisons the libzxv context: fail closed.

A region can also become a *different* accelerator: an image whose header
names another function re-points the region's provider at that function,
and a rollback points it back.

The device is reached only through three callbacks (load, read back,
activate), so the policy knows nothing about any vendor's parts.

## Efficiency: what is claimed and what is measured

An emulator is normally slower than the hardware it imitates, and a
dispatch layer adds a little cost to every call. ZXV's efficiency goal is
narrower than "the emulated machine out-produces its host": it is to get
closer to the host chip's own ceiling than conventional software stacks
do, by keeping work deterministic and skipping work that has already been
done. The benchmark ([`bench_hwm.c`](../../kernel/src/hwemu/bench_hwm.c),
`make -C kernel bench-hwemu`) reports both numbers for each workload:
speed against native code, and the share of a measured per-core ceiling.

One run on a 4-vCPU cloud Xeon at 2.8 GHz (numbers vary by machine; rerun
on yours):

| int8 matrix-vector 64x64 | ns/call | share of ceiling | vs native |
|---|---|---|---|
| native C | 630 | 47% | 1.00x |
| through the matrix | 642 | 46% | 0.98x |

| SHA-256, 32-byte blocks | native | matrix | matrix + memo | native + same cache |
|---|---|---|---|---|
| no repeats | 338 ns | 421 ns | 438 ns | 421 ns |
| 50% repeats | 336 ns | 345 ns | 392 ns | 254 ns |
| 90% repeats | 324 ns | 341 ns | 160 ns | 104 ns |

What this shows:

- The matrix costs about 2% on a compute-heavy call and more on a short
  one. It never beats the native loop doing the same work.
- The memo beats plain native code on repeated inputs (2x at 90% repeats)
  and loses when inputs do not repeat. Native code with the same cache
  wins by the same mechanism, so this is work skipped, not extra
  throughput from the chip.
- The ceiling is measured (dependency-free int16 and int32 multiply-add
  loops on one core), not the vendor's peak. Wider SIMD, more cores and
  matrix engines would raise it.

The phase-tick scheduler and fractal-proportion layouts are ideas to
measure on this same benchmark as they land, not results yet.

## Pre-compatibility with future hardware

ZXV software targets R1, not any particular chip. Hardware built to
satisfy R1, including hardware whose design is private, publishes a ZHP-1
profile and runs the same software with nothing emulated. The profile
format reserves private class and capability ranges so such hardware can
describe its extras without the public format knowing anything about them.
This repository contains no proprietary chip designs, and none are needed
for the software to be ready for them.

## For SDK authors

The libzxv 1.4 calls cover three jobs a hardware-update kit needs:
building signed images (`zxv_hw_image_build`), driving a region
(`zxv_hw_region_*`), and describing and matching machines
(`zxv_hw_profile_*`). Language bindings for these calls, and dedicated
hardware-update kits on top of them, are the next step.
