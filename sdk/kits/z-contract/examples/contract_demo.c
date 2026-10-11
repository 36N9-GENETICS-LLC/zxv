/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* contract_demo.c — a host that enforces examples/fees.zc on the z-wasm
 * rational example plugin.
 *
 *   contract_demo examples/fees.zc rational_plugin.wasm
 *
 * It accepts fee(48000, 125) = 600, refuses fee(-5, 125) before it runs,
 * and stops the plugin when fee(333, 125) proposes 333/80, a fraction the
 * contract does not allow (80 does not divide 100). Then it prints the
 * evidence log and verifies its hash chain. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zcontract_wasm.h"

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *) malloc((size_t) n + 1u);
    if (b && fread(b, 1, (size_t) n, f) != (size_t) n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t) n;
    return b;
}

static void call(zw_plugin *p, zc_wasm_guard *g, int64_t amount, int64_t bps) {
    int64_t args[2] = {amount, bps}, res = 0;
    int32_t st = zw_plugin_call(p, "fee", args, 2, &res, NULL, NULL);
    printf("fee(%lld, %lld): %s\n", (long long) amount, (long long) bps, zw_status_name(st));
    if (st == ZW_E_CHECK) {
        char msg[400];
        int32_t reason = 0, part = 0;
        uint64_t seq = 0;
        zc_wasm_last_violation(g, &part, NULL, &reason, &seq, msg, sizeof msg, NULL);
        printf("  %s %s, evidence #%llu: %s\n", part == ZC_PART_PARAMS ? "refused before running:" : "plugin stopped:",
               zc_reason_name(reason), (unsigned long long) seq, msg);
    }
}

int main(int argc, char **argv) {
    zxv_ctx *ctx;
    zxv_cap *root, *budget_cap;
    zxv_rmag *rm;
    uint32_t bud, line = 0, col = 0;
    zw_host *h;
    zw_plugin *p;
    zc_contract *c;
    zc_evlog *log;
    zc_wasm_guard *g;
    size_t tlen = 0, wlen = 0;
    char msg[400];
    if (argc != 3) {
        fprintf(stderr, "usage: contract_demo fees.zc rational_plugin.wasm\n");
        return 2;
    }
    unsigned char *text = slurp(argv[1], &tlen), *wasm = slurp(argv[2], &wlen);
    if (!text || !wasm) {
        fprintf(stderr, "cannot read the inputs\n");
        return 2;
    }
    if (zc_parse((const char *) text, tlen, &c, &line, &col, NULL, msg, sizeof msg, NULL) != ZXV_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], msg);
        return 1;
    }
    if (zxv_ctx_create(&ctx) || zxv_cap_create(ctx, 1, NULL, ZXV_CAP_ALL, &root) ||
        zxv_cap_attenuate(root, ZXV_CAP_BUDGET, &budget_cap) || zxv_rmag_create_cap(root, &rm) ||
        zxv_rmag_budget(root, rm, "plugins", ZXV_RES_COMPUTE_CYCLES, 1000000, 1, 1, &bud) ||
        zw_host_create(budget_cap, rm, bud, &h) ||
        zw_host_grant(h, ZW_GRANT_RATIONAL | ZW_GRANT_LOGIC | ZW_GRANT_PROPOSE) ||
        zw_plugin_load(h, wasm, wlen, "rational", &p) || zc_evlog_create(1024, &log) ||
        zc_wasm_attach(p, "fee", c, "fee", log, &g)) {
        fprintf(stderr, "setup failed\n");
        return 2;
    }
    call(p, g, 48000, 125);
    call(p, g, -5, 125);
    call(p, g, 333, 125);
    call(p, g, 48000, 125);

    uint32_t pending = 0;
    zw_queue_pending(h, &pending);
    printf("proposals waiting for the host: %u\n", (unsigned) pending);
    uint64_t n = 0;
    zc_evlog_count(log, &n);
    for (uint64_t s = 1; s <= n; s++) {
        int32_t tag = 0;
        uint8_t hash[32];
        char tn[64];
        size_t tl = 0;
        zc_evlog_get(log, s, NULL, NULL, NULL, NULL, &tag, NULL, NULL, hash, msg, sizeof msg, &tl);
        zc_tag_format(tag, tn, sizeof tn, &tl);
        printf("evidence #%llu [%s] %02x%02x%02x%02x...: %s\n", (unsigned long long) s, tn, hash[0],
               hash[1], hash[2], hash[3], msg);
    }
    printf("evidence chain: %s\n", zc_evlog_verify(log) == ZXV_OK ? "verified" : "BROKEN");

    uint64_t id;
    while (zw_queue_next(h, &id) == ZXV_OK) zw_proposal_reject(h, root, id);
    zc_wasm_detach(g);
    zw_plugin_destroy(p);
    zw_host_destroy(h);
    zc_evlog_destroy(log);
    zc_contract_destroy(c);
    zxv_rmag_destroy(rm);
    zxv_cap_destroy(budget_cap);
    zxv_cap_destroy(root);
    zxv_ctx_destroy(ctx);
    free(text);
    free(wasm);
    return 0;
}
