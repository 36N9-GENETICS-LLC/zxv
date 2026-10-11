<!-- Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# hwemu: hardware emulation matrix, profiles and reconfiguration images

| File | What it is |
|---|---|
| `hwp.h/.c` | ZHP-1 hardware profiles ("the hardware ROM") and the reference machine R1 |
| `hwm.h/.c` | the emulation matrix: functions served by FIXED, RECONF, PEER or SOFT providers (M1-M7) |
| `hwr.h/.c` | hardware updates as signed reconfiguration images: two slots, probation, rollback, S0 quarantine (R1-R6) |
| `hwr_mldsa.h/.c` | the ML-DSA-87 binding for image signatures (hosted / pqsec builds) |
| `test_*.c` | host tests (`make test-hwemu`, part of `verify-all`) |
| `bench_hwm.c` | emulated paths against native code and a measured ceiling (`make bench-hwemu`) |

The three policy files are freestanding C11 with no float; `test-hwemu`
builds them with `-mgeneral-regs-only` to keep it that way. Design and
spec: [`sdk/docs/HW_EMULATION.md`](../../../sdk/docs/HW_EMULATION.md),
[`sdk/docs/HW_PROFILE.md`](../../../sdk/docs/HW_PROFILE.md).
