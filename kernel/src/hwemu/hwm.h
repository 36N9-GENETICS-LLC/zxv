/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* hwm.h — the hardware emulation matrix.
 *
 * A FUNCTION is something the system needs done (a hash engine, a codec, a
 * matrix multiplier, a radio modem), named by a 32-bit id. A PROVIDER is
 * one way of doing it, living on one physical RESOURCE. Any function can
 * have several providers of different kinds, and any resource can serve
 * several functions, so the providers form a matrix: functions down the
 * side, resources across the top, one cell per provider.
 *
 *   FIXED   silicon built for this function.
 *   RECONF  a reconfigurable region (FPGA fabric, a microcoded unit)
 *           currently loaded with an image for this function; see hwr.h.
 *   PEER    a component built for another function standing in for this
 *           one (a DSP block serving a codec, a tensor unit serving a
 *           hash). This is "components emulating each other".
 *   SOFT    a software emulator running on a CPU.
 *
 * Callers never pick a provider. They call the FUNCTION (hwm_call) and the
 * matrix binds the best provider that may serve, so a caller cannot tell
 * real hardware, a region and an emulator apart, which is the point.
 *
 * THE RULES
 *   M1 ONE INTERFACE. Every provider of a function has the same call shape
 *      (hwm_call_fn): an opcode, input bytes, output bytes. Nothing else
 *      crosses, so any provider can replace any other.
 *   M2 PROVEN BEFORE BOUND. A provider serves only when its attestation is
 *      TRUE, and TRUE is earned one way: hwm_attest runs the function's
 *      conformance vectors through it and every output matches the
 *      expected bytes exactly. Bit-exact, not "close": the engine is
 *      integer. A mismatch is FALSE, no vectors at all is NEITHER (S0,
 *      unknown never acts), and a disagreement seen in service is BOTH.
 *      Only TRUE binds.
 *   M3 DETERMINISTIC CHOICE. Among the providers that may serve: lowest
 *      cost, then kind (FIXED, RECONF, PEER, SOFT), then the lower index.
 *      Same matrix, same binding, on every machine.
 *   M4 FAILOVER. A provider whose resource faults or starts reconfiguring,
 *      or whose attestation leaves TRUE, is unbound, and the next call
 *      binds the next one. When nothing may serve, the call is refused
 *      (HWM_E_NOPROVIDER); it is never routed to an unproven provider.
 *   M5 DEPENDENCIES ARE DECLARED AND ACYCLIC. A provider may lean on other
 *      functions (a SOFT AES that uses the matrix's "mul" function). It
 *      declares them, may call only those (HWM_E_DEP otherwise), and a
 *      binding is only made when every dependency binds too, without
 *      revisiting a function already on the path and within
 *      HWM_MAX_DEPTH. So A may emulate B and B may emulate A as CELLS,
 *      but no live binding is ever circular.
 *   M6 SHADOW CHECKS. hwm_shadow runs a call on the bound provider and on
 *      the next-best one. If the outputs differ, neither can be trusted
 *      over the other: both go to BOTH, the function stops binding them
 *      and the caller escalates or re-attests. Nothing votes.
 *   M7 MEMO (work skipping). A function the integrator declares PURE (its
 *      output depends only on op and input) may get a caller-supplied memo
 *      table. A top-level call whose op and input match an entry exactly
 *      (full byte compare, no hash-only hits) returns the stored output
 *      without running any provider. The memo is flushed whenever the
 *      binding changes, a vector is added or a shadow check disputes, so a
 *      stored answer always came from the provider that is serving now.
 *      This is the only place the matrix can beat running the work
 *      directly, and only on repeated inputs; bench_hwm.c measures it.
 *
 * HONEST LIMITS. Conformance vectors prove agreement on those vectors, not
 * equivalence on every input; the vectors are the contract and should be
 * chosen like a test suite. Cost is an integer the integrator assigns
 * (cycles, energy units, whatever they measure); the matrix only compares
 * it. Freestanding C11: no libc, no allocation, no float. Not thread-safe:
 * one owner per matrix.
 */
#ifndef HWM_H
#define HWM_H

#include <stdbool.h>
#include <stdint.h>
#include "lpres.h"

#define HWM_MAX_FUNCS     32u
#define HWM_MAX_PROVIDERS 64u
#define HWM_MAX_RESOURCES 32u
#define HWM_MAX_VECTORS   16u
#define HWM_MAX_DEPS      4u
#define HWM_MAX_DEPTH     4u
#define HWM_NAME_LEN      24u
#define HWM_NONE          0xFFFFFFFFu
#define HWM_MEMO_BYTES    64u /* larger inputs or outputs bypass the memo */

typedef enum {
    HWM_KIND_FIXED = 0,
    HWM_KIND_RECONF = 1,
    HWM_KIND_PEER = 2,
    HWM_KIND_SOFT = 3,
    HWM_KIND_COUNT = 4
} hwm_kind_t;

typedef enum {
    HWM_RES_OK = 0,
    HWM_RES_FAULTED = 1,
    HWM_RES_RECONFIGURING = 2,
    HWM_RES_OFFLINE = 3
} hwm_res_state_t;

typedef enum {
    HWM_OK = 0,
    HWM_E_ARG = -1,
    HWM_E_FULL = -2,
    HWM_E_NOT_FOUND = -3,
    HWM_E_DUPLICATE = -4,
    HWM_E_NOPROVIDER = -5, /* M4: nothing proven may serve */
    HWM_E_DEP = -6,        /* M5: undeclared dependency call */
    HWM_E_DEPTH = -7,      /* M5: nesting deeper than HWM_MAX_DEPTH */
    HWM_E_DISPUTE = -8,    /* M6: two providers disagreed */
    HWM_E_PROVIDER = -9,   /* the provider itself returned an error */
    HWM_E_BUFFER = -10     /* output did not fit */
} hwm_status_t;

typedef struct hwm hwm_t;

/* M1. `inst` is the provider's own state. The provider writes at most
 * `out_cap` bytes and the length to *out_len, and returns 0 or a negative
 * error of its own (reported as HWM_E_PROVIDER). It may call hwm_call on
 * `m` for the functions it declared (M5). */
typedef int (*hwm_call_fn)(void *inst, hwm_t *m, uint32_t op, const uint8_t *in, uint32_t in_len,
                           uint8_t *out, uint32_t out_cap, uint32_t *out_len);

typedef struct {
    uint32_t op;
    const uint8_t *in;
    uint32_t in_len;
    const uint8_t *expect;
    uint32_t expect_len;
} hwm_vector_t;

typedef struct {
    bool used;
    uint32_t op, in_len, out_len;
    uint8_t in[HWM_MEMO_BYTES];
    uint8_t out[HWM_MEMO_BYTES];
} hwm_memo_entry_t;

typedef struct {
    uint32_t id;
    char name[HWM_NAME_LEN];
    hwm_vector_t vec[HWM_MAX_VECTORS]; /* bytes stay owned by the caller */
    uint32_t nvec;
    uint32_t bound;     /* provider index, or HWM_NONE */
    uint32_t failovers; /* times a binding was lost and remade */
    uint32_t calls;
    hwm_memo_entry_t *memo; /* M7, caller-owned; NULL = none */
    uint32_t memo_cap;      /* a power of two */
    uint64_t memo_hits, memo_misses;
} hwm_func_t;

typedef struct {
    uint32_t fn;       /* function index */
    uint32_t resource; /* resource index */
    hwm_kind_t kind;
    uint32_t cost;
    hwm_call_fn call;
    void *inst;
    uint32_t deps[HWM_MAX_DEPS]; /* function indices */
    uint32_t ndeps;
    lpres_state_t attest;
    bool enabled;
    uint32_t calls;
    uint64_t cost_spent; /* calls * cost, for measuring (not billing) */
} hwm_provider_t;

typedef struct {
    uint32_t id;
    hwm_res_state_t state;
} hwm_resource_t;

struct hwm {
    hwm_func_t fn[HWM_MAX_FUNCS];
    uint32_t nfn;
    hwm_provider_t pv[HWM_MAX_PROVIDERS];
    uint32_t npv;
    hwm_resource_t res[HWM_MAX_RESOURCES];
    uint32_t nres;
    uint32_t stack[HWM_MAX_DEPTH + 1]; /* providers currently executing */
    uint32_t depth;
};

void hwm_init(hwm_t *m);

/* Functions and resources are named by caller ids; these return the index. */
int hwm_func_add(hwm_t *m, uint32_t id, const char *name, uint32_t *out_index);
int hwm_func_find(const hwm_t *m, uint32_t id, uint32_t *out_index);
/* Add a conformance vector (M2). The bytes must outlive the matrix. Adding
 * a vector resets every provider of the function to NEITHER: the contract
 * changed, so earlier proofs no longer cover it. */
int hwm_vector_add(hwm_t *m, uint32_t fn_index, uint32_t op, const uint8_t *in, uint32_t in_len,
                   const uint8_t *expect, uint32_t expect_len);

int hwm_resource_add(hwm_t *m, uint32_t id, uint32_t *out_index);
int hwm_resource_find(const hwm_t *m, uint32_t id, uint32_t *out_index);
/* M4: anything but OK unbinds every function served on that resource. */
int hwm_resource_set(hwm_t *m, uint32_t res_index, hwm_res_state_t st);

/* A provider starts NEITHER and enabled; it serves after hwm_attest. */
int hwm_provider_add(hwm_t *m, uint32_t fn_index, uint32_t res_index, hwm_kind_t kind,
                     uint32_t cost, hwm_call_fn call, void *inst, uint32_t *out_index);
int hwm_provider_dep(hwm_t *m, uint32_t pv_index, uint32_t dep_fn_index);
int hwm_provider_enable(hwm_t *m, uint32_t pv_index, bool on);
/* Point a provider at another function (a reconfigurable region loaded
 * with an image for something else). Its attestation goes to NEITHER and
 * its dependencies are cleared. */
int hwm_provider_retarget(hwm_t *m, uint32_t pv_index, uint32_t fn_index);
/* Set the attestation directly. Only hwr.h and tests use this; anything
 * but TRUE unbinds. Raising to TRUE this way is how a caller records a
 * proof made elsewhere, so treat it like a signing key. */
int hwm_provider_set_attest(hwm_t *m, uint32_t pv_index, lpres_state_t st);

/* M2: run the vectors through the provider (directly, not via binding)
 * and set its attestation. Returns the new state; *out_failed (optional)
 * is the index of the first vector that did not match, or HWM_NONE. */
lpres_state_t hwm_attest(hwm_t *m, uint32_t pv_index, uint32_t *out_failed);

/* M3/M5: the provider that would serve the function now, or HWM_NONE. */
uint32_t hwm_select(const hwm_t *m, uint32_t fn_index);
/* Bind (or re-bind) and return the provider index in *out_pv. */
int hwm_bind(hwm_t *m, uint32_t fn_index, uint32_t *out_pv);

/* M1/M4/M5: call the function through whatever is bound. */
int hwm_call(hwm_t *m, uint32_t fn_index, uint32_t op, const uint8_t *in, uint32_t in_len,
             uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/* M6: as hwm_call, also running the next-best provider into `scratch`
 * (same capacity as out). If there is no second provider the call is an
 * ordinary hwm_call and *out_checked is false. */
int hwm_shadow(hwm_t *m, uint32_t fn_index, uint32_t op, const uint8_t *in, uint32_t in_len,
               uint8_t *out, uint8_t *scratch, uint32_t out_cap, uint32_t *out_len,
               bool *out_checked);

/* M7: give a PURE function a memo of `cap` entries (a power of two,
 * caller-owned, outliving the matrix). NULL/0 removes it. */
int hwm_memo_attach(hwm_t *m, uint32_t fn_index, hwm_memo_entry_t *entries, uint32_t cap);
void hwm_memo_flush(hwm_t *m, uint32_t fn_index);

const char *hwm_kind_name(hwm_kind_t k);

#endif /* HWM_H */
