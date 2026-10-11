<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ZHP-1: the ZXV hardware profile

A ZXV stack has two halves. The OS image says what the software is. A
**hardware profile** says what the machine is: a tree of abstract
components, each with integer capability descriptors and an optional state
blob. It is the "hardware ROM" that sits beside the OS image, and the
[emulation matrix](HW_EMULATION.md) reads it to decide what runs natively
and what gets emulated.

This page is the normative format. The reference implementation is
[`kernel/src/hwemu/hwp.c`](../../kernel/src/hwemu/hwp.c); the C ABI calls
are `zxv_hw_profile_*` (libzxv 1.4).

## Design goals

1. **One machine, one encoding, one digest.** Canonical rules (P1-P3)
   make the SHA-256 of the encoding the profile's identity, so a profile
   can be signed, pinned and compared byte for byte.
2. **Abstract, not revealing.** A profile names component *classes* and
   *minimum capabilities*. It never describes how a part is built. An
   implementer can publish a profile for proprietary hardware that tells
   the OS exactly what it needs and nothing about the design.
3. **Forward compatible.** Unknown non-critical capabilities are carried
   and ignored; private class and capability ranges let an implementer add
   their own without coordination.
4. **Integers only.** No floating point anywhere in the format.

## Wire format

All integers are little-endian.

```
header (16 bytes)
  0  4  magic "ZHP1"
  4  2  format = 1
  6  2  flags = 0 (reserved)
  8  2  node count (<= 64 in this implementation)
 10  2  reserved = 0
 12  4  total length in bytes, header included

node (repeated `node count` times)
  0  2  id (0..0xFFFE)
  2  2  parent id, or 0xFFFF for a root
  4  4  class
  8  2  cap count (<= 16)
 10  2  state length (<= 256)
 12  .. cap count x { u16 key, u64 value }
 ..  .. state bytes
```

### Canonical rules

- **P1** Node ids strictly increase. A node's parent has a lower id than
  the node (or is 0xFFFF). The tree therefore has no cycles, and parents
  always appear before their children.
- **P2** Within a node, cap keys strictly increase, and no two keys share
  an id (bits 12-0).
- **P3** Reserved fields are zero, and the total length is exact: no
  trailing bytes, no truncation.

A decoder must refuse anything that breaks P1-P3.

## Component classes

| Value | Class | Value | Class |
|---|---|---|---|
| 1 | machine (may nest) | 14 | storage |
| 2 | cpu-cluster | 15 | network |
| 3 | cpu-core | 16 | radio |
| 4 | cache | 17 | sensor |
| 5 | memory | 18 | actuator |
| 6 | interconnect | 19 | clock (deterministic phase-tick source) |
| 7 | matrix-unit (integer MAC arrays) | 20 | power |
| 8 | vector-unit | 21 | secure-element |
| 9 | ternary-unit (trit logic) | 22 | display |
| 10 | hash-unit | 23 | audio |
| 11 | crypto-unit | 24 | wire (UBH-168 harmonic-wire port) |
| 12 | codec-unit | 25 | gpu |
| 13 | reconf-fabric | ≥ 0x80000000 | private (implementer-defined) |

A `machine` node can appear below another node. A soft core loaded into a
reconfigurable region, or a peer on the mesh, is described as a nested
machine with the same format: the description is self-similar at every
scale.

## Capability keys

```
bit 15      CRITICAL
bits 14-13  comparison: 0 MIN (have >= need), 1 EXACT, 2 MASK (all needed
            bits present), 3 INFO (never compared)
bits 12-0   id; 0x1000-0x1FFF are private
```

| Key | Name | Meaning |
|---|---|---|
| 0x0001 | WORD_BITS | native integer width (MIN) |
| 0x0002 | COUNT | cores, lanes or units (MIN) |
| 0x6003 | FREQ_KHZ | clock (INFO) |
| 0x0004 | BYTES | memory, cache or storage size (MIN) |
| 0x6005 | LINE_BYTES | cache line (INFO) |
| 0x2006 | ISA | 1 x86-64, 2 arm64, 3 riscv64, 4 wasm32 (EXACT) |
| 0x4007 | INT_WIDTHS | bit 0 int4, 1 int8, 2 int16, 3 int32, 4 int64, 5 trit (MASK) |
| 0x2008 | RADIX | 2 binary, 3 ternary (EXACT) |
| 0x0009 | MACS_PER_CYCLE | multiply-accumulates per cycle (MIN) |
| 0x400A | SIMD | SSE4.2, AVX2, AVX-512, VNNI, NEON, DOTPROD, I8MM, SVE, RVV (MASK) |
| 0x600B | TICK_NS | deterministic phase-tick period (INFO) |
| 0x000C | REGIONS | reconfigurable regions (MIN) |
| 0x000D | BANDWIDTH_MBPS | (MIN) |
| 0x600E | LATENCY_NS | (INFO) |
| 0x400F | PQ_ALGS | bit 0 ML-KEM, 1 ML-DSA, 2 SLH-DSA (MASK) |
| 0x4010 | HASH_ALGS | bit 0 SHA-256, 1 SHA-3 (MASK) |
| 0x2011 | WIRE_BITS | frame width, 168 for UBH-168 (EXACT) |
| 0x6012 | PEAK_OPS_MHZ | peak integer ops per microsecond, for utilisation reports (INFO) |

**Critical caps.** A requirement whose critical cap the reader does not
know cannot be satisfied. A host component that carries a critical cap the
reader does not know is not used natively ("do not use me blind"). An
implementer's private caps should therefore be non-critical unless the OS
really must understand them.

## Matching a target against a host

A **target** profile describes the machine software is written for; a
**host** profile describes the machine it runs on. For each target node in
id order, the first unused host node of the same class whose capabilities
satisfy every non-INFO requirement is paired with it (**native**).
Unpaired target nodes are **emulated** by the matrix, and the report says
which capability fell short. `machine` nodes are grouping only. The result
is deterministic: the same two profiles always pair the same way.

## The reference machine R1

R1 is the default target and the public contract hardware can be built
to. It asks for minimums only:

| Node | Class | Requirements |
|---|---|---|
| 1 | machine | |
| 2 | cpu-cluster | WORD_BITS ≥ 64, COUNT ≥ 1 |
| 3 | memory | BYTES ≥ 64 MiB |
| 4 | vector-unit | INT_WIDTHS ⊇ {int8, int16, int32} |
| 5 | matrix-unit | INT_WIDTHS ⊇ {int8, int32}, MACS_PER_CYCLE ≥ 64 |
| 6 | ternary-unit | RADIX = 3 |
| 7 | hash-unit | HASH_ALGS ⊇ {SHA-256, SHA-3} |
| 8 | crypto-unit | PQ_ALGS ⊇ {ML-KEM, ML-DSA} |
| 9 | reconf-fabric | REGIONS ≥ 1 |
| 10 | clock | (any deterministic tick source) |
| 11 | secure-element | |
| 12 | wire | WIRE_BITS = 168 |

A commodity laptop satisfies nodes 2-4 natively and emulates 5-12. A
machine whose own profile satisfies all of R1 runs ZXV software with
nothing emulated, and needs no change to that software.

## Host detection

`zxv_hw_profile_probe_host` fills in only what the operating system
reports reliably: the CPU cluster (word size, core count, ISA), memory
size, cache line, and the SIMD extensions the CPU advertises. Units that
cannot be detected portably (matrix engines behind private drivers,
secure enclaves) are left out and therefore emulated. Nothing is guessed.
