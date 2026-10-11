/* Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
/* test_aipi.c — host tests for src/aipi: CIDs, descriptors, GET with a
 * lying peer, EXECUTE over cache / local / mesh, checker, certificate and
 * quorum verification, escrow, expiry and privacy. Several in-process nodes
 * stand in for the mesh; the executor is a small fake sandbox (the real
 * Z-Wasm binding is tested in sdk/kits/aipi). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aipi.h"
#include "../robin_debanks/sha256.h"

static int failures = 0, passes = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (c)                                                                                     \
            passes++;                                                                              \
        else {                                                                                     \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
        }                                                                                          \
    } while (0)

/* ===== a fake sandbox ====================================================== */
/* Functions (by export name), inputs and outputs are u64 little-endian:
 *   isqrt(n)            floor(sqrt(n))       checker isqrt_ok(n, r)
 *   smallest_factor(n)  least prime factor   certifier none (quorum)
 *   composite(n)        1 if n is composite  certifier factor_of, checker composite_ok
 *   salt(n)             n plus a per-node salt (a nondeterministic stand-in) */
typedef struct {
    int lie; /* added to every result (0: honest) */
    uint64_t salt;
    int runs, checks, certs;
} fake_exec_t;

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (8 * i));
}
static uint64_t isqrt(uint64_t n)
{
    uint64_t r = 0;
    for (int b = 31; b >= 0; b--) {
        uint64_t t = r | ((uint64_t) 1 << b);
        if (t * t <= n) r = t;
    }
    return r;
}
static uint64_t least_factor(uint64_t n)
{
    if (n < 2) return n;
    for (uint64_t f = 2; f * f <= n; f++)
        if (n % f == 0) return f;
    return n;
}

static int fx_run(void *ctx, const aipi_code_t *code, const uint8_t *in, uint32_t in_len,
                  uint8_t *out, uint32_t cap, uint32_t *out_len)
{
    fake_exec_t *x = (fake_exec_t *) ctx;
    uint64_t n, r;
    x->runs++;
    if (!code->module || code->module_len == 0) return AIPI_ERR_EXEC;
    if (in_len != 8 || cap < 8) return AIPI_ERR_EXEC;
    n = rd64(in);
    if (!strcmp(code->fn->entry, "isqrt"))
        r = isqrt(n);
    else if (!strcmp(code->fn->entry, "smallest_factor"))
        r = least_factor(n);
    else if (!strcmp(code->fn->entry, "composite"))
        r = (n > 3 && least_factor(n) != n);
    else if (!strcmp(code->fn->entry, "salt"))
        r = n + x->salt;
    else if (!strcmp(code->fn->entry, "trap"))
        return AIPI_ERR_EXEC;
    else
        return AIPI_ERR_EXEC;
    r += (uint64_t) x->lie;
    wr64(out, r);
    *out_len = 8;
    return AIPI_OK;
}

static int fx_check(void *ctx, const aipi_code_t *code, const uint8_t *in, uint32_t in_len,
                    const uint8_t *out, uint32_t out_len, const uint8_t *cert, uint32_t cert_len)
{
    fake_exec_t *x = (fake_exec_t *) ctx;
    uint64_t n, r;
    x->checks++;
    if (in_len != 8 || out_len != 8) return 0;
    n = rd64(in);
    r = rd64(out);
    if (!strcmp(code->fn->checker, "isqrt_ok")) /* the inverse: square it */
        return r * r <= n && (r + 1) * (r + 1) > n;
    if (!strcmp(code->fn->checker, "composite_ok")) {
        uint64_t f;
        if (r != 1) return 0; /* only "composite" carries a certificate */
        if (cert_len != 8) return 0;
        f = rd64(cert);
        return f > 1 && f < n && n % f == 0;
    }
    return AIPI_ERR_EXEC;
}

static int fx_certify(void *ctx, const aipi_code_t *code, const uint8_t *in, uint32_t in_len,
                      const uint8_t *out, uint32_t out_len, uint8_t *cert, uint32_t cap,
                      uint32_t *cert_len)
{
    fake_exec_t *x = (fake_exec_t *) ctx;
    (void) code;
    (void) out;
    (void) out_len;
    x->certs++;
    if (in_len != 8 || cap < 8) return AIPI_ERR_EXEC;
    wr64(cert, least_factor(rd64(in)));
    *cert_len = 8;
    return AIPI_OK;
}

/* ===== the economy: escrow ledger ========================================== */
typedef struct {
    uint64_t balance, escrow, refunded;
    uint64_t paid_to[8];
    uint8_t ids[8][32];
    int nids;
    int fail_reserve;
} ledger_t;

typedef struct node node_t;
static ledger_t *node_ledger(void *ctx);
static int lg_reserve(void *ctx, uint64_t amt)
{
    ledger_t *l = node_ledger(ctx);
    if (l->fail_reserve || amt > l->balance) return -1;
    l->balance -= amt;
    l->escrow += amt;
    return 0;
}
static void lg_pay(void *ctx, const uint8_t ex[32], uint64_t amt)
{
    ledger_t *l = node_ledger(ctx);
    int i;
    for (i = 0; i < l->nids; i++)
        if (!memcmp(l->ids[i], ex, 32)) break;
    if (i == l->nids) {
        memcpy(l->ids[l->nids++], ex, 32);
        l->paid_to[i] = 0;
    }
    l->paid_to[i] += amt;
    l->escrow -= amt;
}
static void lg_refund(void *ctx, uint64_t amt)
{
    ledger_t *l = node_ledger(ctx);
    l->escrow -= amt;
    l->balance += amt;
    l->refunded += amt;
}
static uint64_t lg_paid(const ledger_t *l, const uint8_t ex[32])
{
    for (int i = 0; i < l->nids; i++)
        if (!memcmp(l->ids[i], ex, 32)) return l->paid_to[i];
    return 0;
}

/* ===== a node ============================================================== */
#define JOBS 4
struct node {
    ledger_t lg;
    ipfsn_memstore_t ms;
    ipfsn_storage_ops_t ops;
    ipfsn_bs_entry_t tab[256];
    ipfsn_bs_t bs;
    ipfsn_pin_t pins[32];
    ipfsn_node_t in;
    uint8_t *scratch, *net, *modbuf, *conbuf;
    aipi_job_t jobs[JOBS];
    aipi_memo_t memo[4];
    aipi_ctx_t a;
    fake_exec_t fx;
    lpx_qrec_t qrec[8];
    lpx_qlog_t qlog;
    /* broadcast mailbox */
    uint8_t last_call[AIPI_CALL_ENC_MAX];
    uint32_t last_call_len, last_want, nbroadcasts;
    uint64_t last_bounty;
    /* peer hook: blocks come from `peer`, optionally tampered */
    node_t *peer;
    int tamper, wants;
};

static int nd_broadcast(void *ctx, const ipfsn_cid_t *call, const uint8_t *rec, uint32_t len,
                        uint64_t bounty, uint32_t want)
{
    node_t *n = (node_t *) ctx;
    (void) call;
    memcpy(n->last_call, rec, len);
    n->last_call_len = len;
    n->last_want = want;
    n->last_bounty = bounty;
    n->nbroadcasts++;
    return 0;
}

static ledger_t *node_ledger(void *ctx)
{
    return &((node_t *) ctx)->lg;
}

static int nd_want(void *ctx, const uint8_t *cid_bin, uint32_t cid_len, uint8_t *buf, uint32_t cap,
                   uint32_t *len)
{
    node_t *n = (node_t *) ctx;
    ipfsn_cid_t cid;
    n->wants++;
    if (!n->peer) return -1;
    if (ipfsn_cid_decode_exact(cid_bin, cid_len, &cid) != IPFSN_OK) return -1;
    /* the peer serves as a real node does: never a private block */
    if (ipfsn_serve_block(&n->peer->in, &cid, IPFSN_WIRE_PLAIN, buf, cap, len) != IPFSN_OK)
        return -1;
    if (n->tamper && *len) buf[0] ^= 0x01;
    return 0;
}

static node_t *node_new(uint8_t id)
{
    node_t *n = calloc(1, sizeof *n);
    n->ms.cap = 8u << 20;
    n->ms.buf = malloc(n->ms.cap);
    n->scratch = malloc(IPFSN_SCRATCH_MIN);
    n->net = malloc(IPFSN_BLOCK_MAX + 256);
    n->modbuf = malloc(AIPI_MODULE_MAX);
    n->conbuf = malloc(AIPI_CONTRACT_MAX);
    ipfsn_memstore_ops(&n->ms, &n->ops);
    if (ipfsn_bs_init(&n->bs, &n->ops, n->tab, 256, n->scratch, IPFSN_SCRATCH_MIN) != IPFSN_OK)
        abort();
    {
        uint8_t key[IPFSN_KEY_LEN];
        memset(key, id, sizeof key);
        ipfsn_bs_set_key(&n->bs, key);
    }
    if (ipfsn_node_init(&n->in, &n->bs, n->pins, 32, n->net, IPFSN_BLOCK_MAX + 256) != IPFSN_OK)
        abort();
    n->in.peer.ctx = n;
    n->in.peer.want = nd_want;
    n->in.peer.wire = IPFSN_WIRE_PLAIN;
    if (aipi_init(&n->a, &n->in, n->jobs, JOBS, n->memo, 4, n->modbuf, n->conbuf) != AIPI_OK)
        abort();
    memset(n->a.self, id, 32);
    n->a.online = true;
    n->a.exec.ctx = &n->fx;
    n->a.exec.run = fx_run;
    n->a.exec.check = fx_check;
    n->a.exec.certify = fx_certify;
    n->a.mesh.ctx = n;
    n->a.mesh.broadcast = nd_broadcast;
    lpx_qlog_init(&n->qlog, n->qrec, 8);
    n->a.qlog = &n->qlog;
    return n;
}

static void node_free(node_t *n)
{
    free(n->ms.buf);
    free(n->scratch);
    free(n->net);
    free(n->modbuf);
    free(n->conbuf);
    free(n);
}

/* The requester pays bounties from its own ledger. */
static void use_ledger(node_t *n, uint64_t balance)
{
    memset(&n->lg, 0, sizeof n->lg);
    n->lg.balance = balance;
    n->a.mesh.reserve = lg_reserve;
    n->a.mesh.pay = lg_pay;
    n->a.mesh.refund = lg_refund;
}

/* ===== shared fixtures ===================================================== */
static const uint8_t MODULE[] = "fake-module: isqrt smallest_factor composite salt trap";
static const uint8_t CONTRACT[] = "fn isqrt(n: int[0..2^62]) -> int[0..2^31];";

static void make_fn(aipi_fn_t *fn, const ipfsn_cid_t *mod, const char *entry, int verify)
{
    memset(fn, 0, sizeof *fn);
    fn->intent = AIPI_INTENT_FN;
    fn->verify = (uint8_t) verify;
    fn->privacy = AIPI_PRIV_PUBLIC;
    fn->determinism = AIPI_DET_DETERMINISTIC;
    fn->fuel_max = 100000;
    fn->module = *mod;
    strcpy(fn->entry, entry);
    if (verify == AIPI_VERIFY_QUORUM) {
        fn->quorum_n = 3;
        fn->quorum_k = 2;
    }
}

static uint8_t in8[8];
static const uint8_t *u64in(uint64_t v)
{
    wr64(in8, v);
    return in8;
}

/* Executor `e` answers what requester `r` last broadcast; returns claim len. */
static int answer(node_t *e, node_t *r, uint8_t *claim, uint32_t cap)
{
    return aipi_serve(&e->a, r->last_call, r->last_call_len, claim, cap);
}

/* ===== tests ================================================================ */

static void test_cids_and_text(void)
{
    node_t *n = node_new(1);
    ipfsn_cid_t mod, f, f2, call, back;
    aipi_fn_t fn;
    char txt[AIPI_TEXT_MAX], txt2[AIPI_TEXT_MAX];
    uint8_t k1[32], k2[32];
    int len;

    CHECK(aipi_put_blob(&n->a, MODULE, sizeof MODULE, IPFSN_VIS_PUBLIC, &mod) == AIPI_OK);
    CHECK(mod.codec == IPFSN_MC_RAW && mod.version == 1);
    make_fn(&fn, &mod, "isqrt", AIPI_VERIFY_CHECKER);
    strcpy(fn.checker, "isqrt_ok");
    CHECK(aipi_fn_cid(&fn, &f) == AIPI_OK);
    CHECK(f.codec == AIPI_MC_FN && f.version == 1 && f.mh_code == IPFSN_MH_SHA2_256);
    fn.intent = AIPI_INTENT_TENSOR_INFER;
    CHECK(aipi_fn_cid(&fn, &f2) == AIPI_OK && f2.codec == AIPI_MC_TENSOR_INFER);
    CHECK(memcmp(f.digest, f2.digest, 32) != 0); /* the intent is inside the hash too */
    CHECK(aipi_intent_codec(AIPI_INTENT_SETTLE_LEDGER) == 0x3A1002u);
    CHECK(aipi_intent_codec(AIPI_INTENT_SWARM_ROUTE) == 0x3A1003u);
    CHECK(aipi_intent_codec(AIPI_INTENT_COUNT) == 0);
    CHECK(aipi_is_fn_codec(AIPI_MC_FN) && aipi_is_fn_codec(AIPI_MC_SWARM_ROUTE));
    CHECK(!aipi_is_fn_codec(AIPI_MC_CALL) && !aipi_is_fn_codec(IPFSN_MC_RAW) &&
          !aipi_is_fn_codec(AIPI_MC_FN + AIPI_INTENT_COUNT) && !aipi_is_fn_codec(AIPI_MC_FN - 1));
    /* every AIPI code is in the multicodec private-use range */
    CHECK(AIPI_MC_FN >= 0x300000u && AIPI_MC_CALL <= 0x3FFFFFu);

    len = aipi_cid_to_text(&f, txt, sizeof txt);
    CHECK(len > 1 && txt[0] == 'z' && (int) strlen(txt) == len);
    CHECK(aipi_cid_from_text(txt, (uint32_t) len, &back) == AIPI_OK && ipfsn_cid_equal(&back, &f));
    /* the base32 form of the same CID parses to the same CID */
    CHECK(ipfsn_cid_to_string(&f, txt2, sizeof txt2) > 0 && txt2[0] == 'b');
    CHECK(aipi_cid_from_text(txt2, (uint32_t) strlen(txt2), &back) == AIPI_OK &&
          ipfsn_cid_equal(&back, &f));
    /* one character changed: a different CID or a refusal, never the same */
    txt[len - 1] = txt[len - 1] == '2' ? '3' : '2';
    CHECK(aipi_cid_from_text(txt, (uint32_t) len, &back) != AIPI_OK || !ipfsn_cid_equal(&back, &f));
    CHECK(aipi_cid_from_text("Qm", 2, &back) == AIPI_ERR_UNSUPP);
    CHECK(aipi_cid_from_text("z", 1, &back) == AIPI_ERR_MALFORMED);
    {
        /* a leading '1' adds a zero byte: not the canonical text of any CID */
        char bad[AIPI_TEXT_MAX];
        len = aipi_cid_to_text(&f, txt, sizeof txt);
        bad[0] = 'z';
        bad[1] = '1';
        memcpy(bad + 2, txt + 1, (size_t) len - 1);
        CHECK(aipi_cid_from_text(bad, (uint32_t) len + 1, &back) == AIPI_ERR_MALFORMED);
    }
    CHECK(aipi_cid_to_text(&f, txt, 4) == AIPI_ERR_SPACE);

    /* calls: same function + same input = same CID; any change = another */
    {
        ipfsn_cid_t c2, c3;
        uint8_t a[3] = {1, 2, 3}, b[3] = {1, 2, 4};
        CHECK(aipi_call_cid(&f, a, 3, &call) == AIPI_OK && call.codec == AIPI_MC_CALL);
        CHECK(aipi_call_cid(&f, a, 3, &c2) == AIPI_OK && ipfsn_cid_equal(&call, &c2));
        CHECK(aipi_call_cid(&f, b, 3, &c3) == AIPI_OK && !ipfsn_cid_equal(&call, &c3));
        CHECK(aipi_call_cid(&f2, a, 3, &c3) == AIPI_OK && !ipfsn_cid_equal(&call, &c3));
        CHECK(aipi_call_cid(&f, a, 2, &c3) == AIPI_OK && !ipfsn_cid_equal(&call, &c3));
        CHECK(aipi_call_cid(&mod, a, 3, &c3) == AIPI_ERR_UNSUPP); /* data CID is no function */
    }

    CHECK(aipi_provider_key(&f, k1) == AIPI_OK);
    CHECK(aipi_provider_key(&f2, k2) == AIPI_OK && memcmp(k1, k2, 32) != 0);
    {
        /* SHA-256("zxv/aipi/v1/provider" || cid) exactly */
        uint8_t b[IPFSN_CID_BIN_MAX + 32], d[32];
        int m = ipfsn_cid_encode(&f, b + 20, IPFSN_CID_BIN_MAX);
        memcpy(b, "zxv/aipi/v1/provider", 20);
        sha256(b, (size_t) (20 + m), d);
        CHECK(memcmp(d, k1, 32) == 0);
        CHECK(memcmp(d, f.digest, 32) != 0); /* not the bare CID digest */
    }
    node_free(n);
}

static void test_descriptor_rules(void)
{
    ipfsn_cid_t mod, notraw;
    aipi_fn_t fn, back, bad;
    uint8_t enc[AIPI_FN_ENC_MAX], tmp[AIPI_FN_ENC_MAX];
    int len;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    ipfsn_cid_sha256(IPFSN_MC_DAG_PB, MODULE, sizeof MODULE, &notraw);
    make_fn(&fn, &mod, "composite", AIPI_VERIFY_CERT);
    strcpy(fn.checker, "composite_ok");
    strcpy(fn.certifier, "factor_of");
    fn.has_contract = true;
    ipfsn_cid_sha256(IPFSN_MC_RAW, CONTRACT, sizeof CONTRACT, &fn.contract);
    fn.fuel_max = 0x0102030405060708ull;
    CHECK(aipi_fn_check(&fn) == AIPI_OK);
    len = aipi_fn_encode(&fn, enc, sizeof enc);
    CHECK(len > 19);
    CHECK(aipi_fn_decode(enc, (uint32_t) len, &back) == AIPI_OK);
    CHECK(back.fuel_max == 0x0102030405060708ull && back.has_contract &&
          ipfsn_cid_equal(&back.contract, &fn.contract) && !strcmp(back.certifier, "factor_of") &&
          !strcmp(back.checker, "composite_ok") && !strcmp(back.entry, "composite") &&
          back.verify == AIPI_VERIFY_CERT && ipfsn_cid_equal(&back.module, &mod));
    CHECK(enc[11] == 0x08 && enc[18] == 0x01); /* fuel little-endian at bytes 11..18 */
    CHECK(aipi_fn_encode(&fn, enc, (uint32_t) len - 1) == AIPI_ERR_SPACE);

    /* every truncation and one trailing byte are refused */
    {
        int bad_n = 0;
        for (int i = 0; i < len; i++)
            if (aipi_fn_decode(enc, (uint32_t) i, &back) == AIPI_OK) bad_n++;
        CHECK(bad_n == 0);
        memcpy(tmp, enc, (size_t) len);
        tmp[len] = 0;
        CHECK(aipi_fn_decode(tmp, (uint32_t) len + 1, &back) == AIPI_ERR_MALFORMED);
    }
    /* every single-bit flip is refused or decodes to a DIFFERENT descriptor
     * that re-encodes to exactly the flipped bytes (one function, one CID) */
    {
        int bad_n = 0;
        for (int i = 0; i < len; i++)
            for (int b = 0; b < 8; b++) {
                memcpy(tmp, enc, (size_t) len);
                tmp[i] ^= (uint8_t) (1u << b);
                if (aipi_fn_decode(tmp, (uint32_t) len, &back) == AIPI_OK) {
                    uint8_t again[AIPI_FN_ENC_MAX];
                    int m = aipi_fn_encode(&back, again, sizeof again);
                    if (m != len || memcmp(again, tmp, (size_t) len)) bad_n++;
                }
            }
        CHECK(bad_n == 0);
    }
    /* specific refusals */
    enc[0] = 'X';
    CHECK(aipi_fn_decode(enc, (uint32_t) len, &back) == AIPI_ERR_MALFORMED);
    enc[0] = 'A';
    enc[4] = 2; /* version */
    CHECK(aipi_fn_decode(enc, (uint32_t) len, &back) == AIPI_ERR_MALFORMED);
    enc[4] = 1;

#define REFUSE(stmt)                                                                               \
    do {                                                                                           \
        bad = fn;                                                                                  \
        stmt;                                                                                      \
        CHECK(aipi_fn_check(&bad) == AIPI_ERR_MALFORMED);                                          \
        CHECK(aipi_fn_encode(&bad, tmp, sizeof tmp) == AIPI_ERR_MALFORMED);                        \
    } while (0)
    REFUSE(bad.intent = AIPI_INTENT_COUNT);
    REFUSE(bad.privacy = 2);
    REFUSE(bad.determinism = 2);
    REFUSE(bad.verify = 0);
    REFUSE(bad.verify = 5);
    REFUSE(bad.fuel_max = 0);
    REFUSE(bad.module = notraw);
    REFUSE(bad.contract = notraw);
    REFUSE(bad.entry[0] = 0);
    REFUSE(strcpy(bad.entry, "bad-name"));
    REFUSE(strcpy(bad.entry, "bad name"));
    REFUSE(bad.checker[0] = 0);                    /* CERT needs a checker   */
    REFUSE(bad.certifier[0] = 0);                  /* CERT needs a certifier */
    REFUSE(strcpy(bad.checker, "composite"));      /* checker != entry       */
    REFUSE(strcpy(bad.certifier, "composite_ok")); /* certifier != checker   */
    REFUSE(strcpy(bad.certifier, "composite"));    /* certifier != entry     */
    REFUSE(bad.quorum_n = 3);                      /* quorum fields only for QUORUM */
    REFUSE((bad.verify = AIPI_VERIFY_CHECKER));    /* CHECKER takes no certifier */
    REFUSE((bad.verify = AIPI_VERIFY_LOCAL, bad.certifier[0] = 0)); /* LOCAL takes no checker */
    /* a full 63-character name is fine, 64 is not */
    bad = fn;
    memset(bad.entry, 'a', 63);
    bad.entry[63] = 0;
    CHECK(aipi_fn_check(&bad) == AIPI_OK);
    CHECK(aipi_fn_encode(&bad, tmp, sizeof tmp) > 0);
    CHECK(aipi_fn_decode(tmp, (uint32_t) aipi_fn_encode(&bad, tmp, sizeof tmp), &back) == AIPI_OK);

    /* quorum rules: deterministic, 2 <= n <= 8, n/2 < k <= n */
    make_fn(&fn, &mod, "smallest_factor", AIPI_VERIFY_QUORUM);
    CHECK(aipi_fn_check(&fn) == AIPI_OK);
    REFUSE(bad.determinism = AIPI_DET_NONDETERMINISTIC);
    REFUSE(bad.quorum_k = 1);                     /* 2k <= n  */
    REFUSE((bad.quorum_n = 4, bad.quorum_k = 2)); /* 2k == n: two groups could tie */
    REFUSE(bad.quorum_k = 4);                     /* k > n    */
    REFUSE((bad.quorum_n = 1, bad.quorum_k = 1));
    REFUSE((bad.quorum_n = 9, bad.quorum_k = 9));
    REFUSE(strcpy(bad.checker, "isqrt_ok"));
    bad = fn;
    bad.quorum_n = 8;
    bad.quorum_k = 5;
    CHECK(aipi_fn_check(&bad) == AIPI_OK);
    bad.quorum_n = 2;
    bad.quorum_k = 2;
    CHECK(aipi_fn_check(&bad) == AIPI_OK);
    /* LOCAL may be nondeterministic; CHECKER too */
    make_fn(&fn, &mod, "salt", AIPI_VERIFY_LOCAL);
    fn.determinism = AIPI_DET_NONDETERMINISTIC;
    CHECK(aipi_fn_check(&fn) == AIPI_OK);
#undef REFUSE
    CHECK(aipi_fn_check(NULL) == AIPI_ERR_ARG);
}

static void test_records(void)
{
    ipfsn_cid_t mod, f, call, fc;
    aipi_fn_t fn;
    uint8_t rec[AIPI_CALL_ENC_MAX], claim[AIPI_CLAIM_ENC_MAX], ex[32], big[AIPI_IO_MAX + 1];
    const uint8_t *in;
    uint32_t in_len;
    aipi_claim_t cl;
    int len;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&fn, &mod, "isqrt", AIPI_VERIFY_CHECKER);
    strcpy(fn.checker, "isqrt_ok");
    aipi_fn_cid(&fn, &f);
    len = aipi_call_encode(&f, (const uint8_t *) "abc", 3, rec, sizeof rec);
    CHECK(len > 0);
    CHECK(aipi_call_decode(rec, (uint32_t) len, &fc, &in, &in_len) == AIPI_OK);
    CHECK(ipfsn_cid_equal(&fc, &f) && in_len == 3 && !memcmp(in, "abc", 3));
    CHECK(aipi_call_decode(rec, (uint32_t) len - 1, &fc, &in, &in_len) == AIPI_ERR_MALFORMED);
    rec[5] = 1; /* reserved flags */
    CHECK(aipi_call_decode(rec, (uint32_t) len, &fc, &in, &in_len) == AIPI_ERR_MALFORMED);
    rec[5] = 0;
    memset(big, 7, sizeof big);
    CHECK(aipi_call_encode(&f, big, AIPI_IO_MAX + 1, rec, sizeof rec) == AIPI_ERR_SPACE);
    CHECK(aipi_call_encode(&f, big, AIPI_IO_MAX, rec, sizeof rec) ==
          6 + 1 + 39 + 2 + (int) AIPI_IO_MAX); /* fn CID: 1 + 4 (codec varint) + 2 + 32 */

    aipi_call_cid(&f, (const uint8_t *) "abc", 3, &call);
    memset(ex, 0xAB, 32);
    len = aipi_claim_encode(&call, ex, (const uint8_t *) "out", 3, (const uint8_t *) "ce", 2, claim,
                            sizeof claim);
    CHECK(len > 0);
    CHECK(aipi_claim_decode(claim, (uint32_t) len, &cl) == AIPI_OK);
    CHECK(ipfsn_cid_equal(&cl.call, &call) && !memcmp(cl.executor, ex, 32) && cl.out_len == 3 &&
          !memcmp(cl.out, "out", 3) && cl.cert_len == 2 && !memcmp(cl.cert, "ce", 2));
    {
        int bad_n = 0;
        for (int i = 0; i < len; i++)
            if (aipi_claim_decode(claim, (uint32_t) i, &cl) == AIPI_OK) bad_n++;
        CHECK(bad_n == 0);
    }
    claim[len] = 0;
    CHECK(aipi_claim_decode(claim, (uint32_t) len + 1, &cl) == AIPI_ERR_MALFORMED);
    /* a claim must answer a CALL CID, not a function or data CID */
    CHECK(aipi_claim_encode(&f, ex, 0, 0, 0, 0, claim, sizeof claim) == AIPI_ERR_UNSUPP);
    CHECK(aipi_claim_encode(&call, ex, big, AIPI_IO_MAX + 1, 0, 0, claim, sizeof claim) ==
          AIPI_ERR_SPACE);
    CHECK(aipi_claim_encode(&call, ex, big, 0, big, AIPI_CERT_MAX + 1, claim, sizeof claim) ==
          AIPI_ERR_SPACE);
}

static void test_get(void)
{
    node_t *r = node_new(1), *p = node_new(2);
    ipfsn_cid_t data, priv, missing;
    uint8_t buf[256];
    uint32_t len = 0;
    lpx_t tag;
    static const uint8_t DATA[] = "weights shard 7";
    static const uint8_t SECRET[] = "private key-bearing note";

    r->peer = p;
    CHECK(aipi_put_blob(&p->a, DATA, sizeof DATA, IPFSN_VIS_PUBLIC, &data) == AIPI_OK);
    CHECK(aipi_put_blob(&r->a, SECRET, sizeof SECRET, IPFSN_VIS_PRIVATE, &priv) == AIPI_OK);
    ipfsn_cid_sha256(IPFSN_MC_RAW, (const uint8_t *) "nobody has this", 15, &missing);

    /* offline: only the local cache */
    r->a.online = false;
    CHECK(aipi_get(&r->a, &data, buf, sizeof buf, &len, &tag) == AIPI_ERR_OFFLINE);
    CHECK(r->wants == 0);
    CHECK(lpx_base(tag) == LPX_B_NEITHER);
    r->a.online = true;

    /* a lying peer: the bytes are dropped as FORBIDDEN and logged */
    r->tamper = 1;
    CHECK(aipi_get(&r->a, &data, buf, sizeof buf, &len, &tag) == AIPI_ERR_HASH);
    CHECK(lpx_forbidden(tag) && lpx_action(tag) == LPX_ACT_ROLLBACK);
    CHECK(r->a.forbidden == 1 && r->qlog.n == 1 && lpx_qlog_verify(&r->qlog));
    CHECK(r->qlog.rec[0].submitter == 2 && !memcmp(r->qlog.rec[0].subject, data.digest, 32));
    CHECK(!ipfsn_bs_has(&r->bs, &data)); /* nothing tampered was stored */

    /* an honest peer: verified, then cached */
    r->tamper = 0;
    CHECK(aipi_get(&r->a, &data, buf, sizeof buf, &len, &tag) == AIPI_OK);
    CHECK(len == sizeof DATA && !memcmp(buf, DATA, len));
    CHECK(lpx_base(tag) == LPX_B_TRUE && lpx_phase(tag) == LPX_PH_MANIFEST &&
          lpx_action(tag) == LPX_ACT_PROCEED);
    CHECK(ipfsn_bs_has(&r->bs, &data));
    {
        int w = r->wants;
        r->a.online = false; /* now it works offline too */
        CHECK(aipi_get(&r->a, &data, buf, sizeof buf, &len, &tag) == AIPI_OK);
        CHECK(r->wants == w);
        r->a.online = true;
    }
    /* nobody has it */
    CHECK(aipi_get(&r->a, &missing, buf, sizeof buf, &len, &tag) == AIPI_ERR_NOTFOUND);
    /* private: served from the local sealed store, never asked of peers */
    {
        int w = r->wants;
        CHECK(aipi_get(&r->a, &priv, buf, sizeof buf, &len, &tag) == AIPI_OK);
        CHECK(len == sizeof SECRET && !memcmp(buf, SECRET, len));
        CHECK(r->wants == w);
    }
    /* the peer does not know r's private block, and is not asked */
    p->peer = r;
    {
        int w = r->wants;
        CHECK(aipi_get(&p->a, &priv, buf, sizeof buf, &len, &tag) == AIPI_ERR_NOTFOUND);
        CHECK(r->wants == w && p->wants == 1 && p->a.forbidden == 0);
    }
    /* identity multihashes prove nothing and are not fetched */
    {
        ipfsn_cid_t id;
        ipfsn_cid_identity(IPFSN_MC_RAW, (const uint8_t *) "x", 1, &id);
        CHECK(aipi_get(&r->a, &id, buf, sizeof buf, &len, &tag) == AIPI_ERR_UNSUPP);
    }
    CHECK(aipi_get(&r->a, &data, buf, 3, &len, &tag) == AIPI_ERR_SPACE);
    node_free(r);
    node_free(p);
}

/* A requester with the descriptor but maybe not the code. */
static void publish(node_t *home, const aipi_fn_t *fn, ipfsn_cid_t *f)
{
    ipfsn_cid_t mod;
    CHECK(aipi_put_blob(&home->a, MODULE, sizeof MODULE, IPFSN_VIS_PUBLIC, &mod) == AIPI_OK);
    CHECK(aipi_publish_fn(&home->a, fn, f) == AIPI_OK);
}

static void test_local_offline_and_fetch(void)
{
    node_t *r = node_new(1), *p = node_new(2);
    ipfsn_cid_t mod, f;
    aipi_fn_t fn;
    uint32_t job, len;
    uint8_t out[16];
    const aipi_job_t *j;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&fn, &mod, "isqrt", AIPI_VERIFY_CHECKER);
    strcpy(fn.checker, "isqrt_ok");

    /* code only on the provider; requester has nothing, offline: unavailable */
    publish(p, &fn, &f);
    r->peer = p;
    r->a.online = false;
    CHECK(aipi_execute(&r->a, &f, u64in(1000), 8, false, 0, 0, 100, &job) == AIPI_ERR_OFFLINE);
    CHECK(r->wants == 0 && r->nbroadcasts == 0);
    CHECK(aipi_job(&r->a, 0) == NULL); /* no job left behind */

    /* online, prefer local: fetch descriptor + code by CID, run here */
    r->a.online = true;
    r->a.prefer_local = true;
    CHECK(aipi_execute(&r->a, &f, u64in(1000), 8, false, 0, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    CHECK(j && j->status == AIPI_JOB_DONE && j->route == AIPI_ROUTE_FETCH_LOCAL);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && len == 8 && rd64(out) == 31);
    CHECK(lpx_base(j->tag) == LPX_B_TRUE && lpx_phase(j->tag) == LPX_PH_MANIFEST);
    CHECK(r->nbroadcasts == 0 && r->fx.runs == 1);
    CHECK(aipi_release(&r->a, job) == AIPI_OK && aipi_job(&r->a, job) == NULL);

    /* now cached: the network goes down and the call still works (A3) */
    r->a.online = false;
    r->a.prefer_local = false;
    CHECK(aipi_execute(&r->a, &f, u64in(99), 8, false, 0, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    CHECK(j->route == AIPI_ROUTE_LOCAL && j->status == AIPI_JOB_DONE);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 9);
    aipi_release(&r->a, job);

    /* a verified deterministic result is memoized under its call CID */
    {
        int runs = r->fx.runs;
        CHECK(aipi_execute(&r->a, &f, u64in(99), 8, false, 0, 0, 100, &job) == AIPI_OK);
        j = aipi_job(&r->a, job);
        CHECK(j->route == AIPI_ROUTE_CACHE && r->fx.runs == runs);
        CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 9);
        CHECK(lpx_base(j->tag) == LPX_B_TRUE && lpx_phase(j->tag) == LPX_PH_MANIFEST);
        aipi_release(&r->a, job);
    }
    /* a sandbox failure is FAILED / FALSE, not a result */
    {
        aipi_fn_t t;
        ipfsn_cid_t tf;
        make_fn(&t, &mod, "trap", AIPI_VERIFY_LOCAL);
        CHECK(aipi_publish_fn(&r->a, &t, &tf) == AIPI_OK);
        CHECK(aipi_execute(&r->a, &tf, u64in(1), 8, false, 0, 0, 100, &job) == AIPI_ERR_EXEC);
        j = aipi_job(&r->a, job);
        CHECK(j && j->status == AIPI_JOB_FAILED && lpx_base(j->tag) == LPX_B_FALSE);
        CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_ERR_STATE);
        aipi_release(&r->a, job);
    }
    /* the table fills */
    {
        uint32_t ids[JOBS];
        for (uint32_t i = 0; i < JOBS; i++)
            CHECK(aipi_execute(&r->a, &f, u64in(i + 500), 8, false, 0, 0, 100, &ids[i]) == AIPI_OK);
        CHECK(aipi_execute(&r->a, &f, u64in(7), 8, false, 0, 0, 100, &job) == AIPI_ERR_FULL);
        for (uint32_t i = 0; i < JOBS; i++) aipi_release(&r->a, ids[i]);
    }
    CHECK(aipi_execute(&r->a, &f, in8, AIPI_IO_MAX + 1, false, 0, 0, 100, &job) == AIPI_ERR_SPACE);
    node_free(r);
    node_free(p);
}

static void test_mesh_checker(void)
{
    node_t *r = node_new(1), *liar = node_new(3), *hon = node_new(4), *late = node_new(5);
    ipfsn_cid_t f;
    aipi_fn_t fn;
    ipfsn_cid_t mod;
    uint8_t claim[AIPI_CLAIM_ENC_MAX], v = 0, out[16];
    uint32_t job, len;
    int cl;
    const aipi_job_t *j;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&fn, &mod, "isqrt", AIPI_VERIFY_CHECKER);
    strcpy(fn.checker, "isqrt_ok");
    publish(r, &fn, &f);
    publish(liar, &fn, &f);
    publish(hon, &fn, &f);
    publish(late, &fn, &f);
    liar->fx.lie = 1;
    use_ledger(r, 1000);

    CHECK(aipi_execute(&r->a, &f, u64in(1u << 20), 8, false, 300, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    CHECK(j->status == AIPI_JOB_PENDING && j->route == AIPI_ROUTE_MESH);
    CHECK(r->nbroadcasts == 1 && r->last_want == 1 && r->last_bounty == 300);
    CHECK(r->lg.balance == 700 && r->lg.escrow == 300);
    CHECK(lpx_phase(j->tag) == LPX_PH_EMERGING && lpx_base(j->tag) == LPX_B_NEITHER);
    CHECK(lpx_action(j->tag) == LPX_ACT_WAIT);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_ERR_STATE);
    CHECK(aipi_release(&r->a, job) == AIPI_ERR_STATE); /* escrow still held */

    /* a wrong answer fails the inverse check: not paid, job BOTH */
    cl = answer(liar, r, claim, sizeof claim);
    CHECK(cl > 0);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_REJECTED);
    CHECK(lpx_base(j->tag) == LPX_B_BOTH && lpx_phase(j->tag) == LPX_PH_EMERGING);
    CHECK(j->status == AIPI_JOB_PENDING && j->nrejected == 1);
    CHECK(r->lg.escrow == 300 && lg_paid(&r->lg, liar->a.self) == 0);
    /* the same executor cannot try again */
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_ERR_DUP);
    /* a tampered claim (bytes cut) is malformed, not counted */
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl - 1, &v) == AIPI_ERR_MALFORMED);

    /* the right answer verifies: paid in full, job TRUE / manifest */
    cl = answer(hon, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_ACCEPTED);
    CHECK(j->status == AIPI_JOB_DONE && j->naccepted == 1);
    CHECK(lpx_base(j->tag) == LPX_B_TRUE && lpx_phase(j->tag) == LPX_PH_MANIFEST &&
          lpx_action(j->tag) == LPX_ACT_PROCEED);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 1024);
    CHECK(lg_paid(&r->lg, hon->a.self) == 300 && r->lg.escrow == 0 && r->lg.refunded == 0);
    CHECK(j->paid == 300 && j->refunded == 0);
    CHECK(r->fx.checks == 2); /* each claim checked once, locally */
    /* too late: the job is closed, nobody else is paid */
    cl = answer(late, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_ERR_STATE);
    CHECK(lg_paid(&r->lg, late->a.self) == 0);
    CHECK(aipi_release(&r->a, job) == AIPI_OK);

    /* the same call again: answered from the memo, no broadcast, no bounty */
    CHECK(aipi_execute(&r->a, &f, u64in(1u << 20), 8, false, 300, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    CHECK(j->route == AIPI_ROUTE_CACHE && r->nbroadcasts == 1 && r->lg.balance == 700);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 1024);
    aipi_release(&r->a, job);

    /* a claim for another call is refused */
    CHECK(aipi_execute(&r->a, &f, u64in(50), 8, false, 10, 0, 100, &job) == AIPI_OK);
    {
        uint8_t other[AIPI_CALL_ENC_MAX];
        int ol = aipi_call_encode(&f, u64in(51), 8, other, sizeof other);
        cl = aipi_serve(&hon->a, other, (uint32_t) ol, claim, sizeof claim);
        CHECK(cl > 0 && aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_ERR_ARG);
    }
    CHECK(aipi_submit(&r->a, JOBS, claim, (uint32_t) cl, &v) == AIPI_ERR_ARG);
    CHECK(aipi_tick(&r->a, 99) == 0 && aipi_tick(&r->a, 100) == 1);
    CHECK(r->lg.balance == 700 && r->lg.escrow == 0);
    aipi_release(&r->a, job);

    /* CHECKER mode needs the checker's code before anyone works for us */
    {
        node_t *bare = node_new(9);
        ipfsn_cid_t f2;
        use_ledger(bare, 100);
        CHECK(aipi_publish_fn(&bare->a, &fn, &f2) == AIPI_OK && ipfsn_cid_equal(&f2, &f));
        CHECK(aipi_execute(&bare->a, &f, u64in(4), 8, false, 10, 0, 100, &job) ==
              AIPI_ERR_NOTFOUND);
        CHECK(bare->nbroadcasts == 0 && bare->lg.balance == 100);
        CHECK(aipi_job(&bare->a, 0) == NULL);
        node_free(bare);
    }
    node_free(r);
    node_free(liar);
    node_free(hon);
    node_free(late);
}

static void test_mesh_cert(void)
{
    node_t *r = node_new(1), *e = node_new(4);
    ipfsn_cid_t mod, f, call;
    aipi_fn_t fn;
    uint8_t claim[AIPI_CLAIM_ENC_MAX], v = 0, one[8];
    uint32_t job;
    int cl;
    const aipi_job_t *j;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&fn, &mod, "composite", AIPI_VERIFY_CERT);
    strcpy(fn.checker, "composite_ok");
    strcpy(fn.certifier, "factor_of");
    publish(r, &fn, &f);
    publish(e, &fn, &f);
    use_ledger(r, 50);

    CHECK(aipi_execute(&r->a, &f, u64in(91), 8, false, 7, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    aipi_call_cid(&f, u64in(91), 8, &call);
    /* "composite" with no certificate is refused before any check runs */
    wr64(one, 1);
    cl = aipi_claim_encode(&call, (const uint8_t *) "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", one, 8, 0,
                           0, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_REJECTED);
    CHECK(r->fx.checks == 0);
    /* a certificate that is not a factor is refused by the checker */
    {
        uint8_t bad[8];
        wr64(bad, 5);
        cl = aipi_claim_encode(&call, (const uint8_t *) "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", one, 8,
                               bad, 8, claim, sizeof claim);
        CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_REJECTED);
        CHECK(r->fx.checks == 1 && j->nrejected == 2);
    }
    /* the executor's real certificate (7 divides 91) verifies */
    cl = answer(e, r, claim, sizeof claim);
    CHECK(cl > 0 && e->fx.certs == 1);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_ACCEPTED);
    CHECK(lg_paid(&r->lg, e->a.self) == 7 && r->lg.balance == 43);
    CHECK(j->status == AIPI_JOB_DONE);
    node_free(r);
    node_free(e);
}

static void test_mesh_quorum(void)
{
    node_t *r = node_new(1), *a = node_new(4), *b = node_new(5), *liar = node_new(3);
    ipfsn_cid_t mod, f;
    aipi_fn_t fn;
    uint8_t claim[AIPI_CLAIM_ENC_MAX], v = 0, out[16];
    uint32_t job, len;
    int cl;
    const aipi_job_t *j;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&fn, &mod, "smallest_factor", AIPI_VERIFY_QUORUM);
    /* the requester holds only the descriptor: quorum needs no local code */
    CHECK(aipi_publish_fn(&r->a, &fn, &f) == AIPI_OK);
    publish(a, &fn, &f);
    publish(b, &fn, &f);
    publish(liar, &fn, &f);
    liar->fx.lie = 1;
    use_ledger(r, 1000);

    CHECK(aipi_execute(&r->a, &f, u64in(221), 8, false, 101, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    CHECK(r->last_want == 3 && j->route == AIPI_ROUTE_MESH);

    cl = answer(a, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_COUNTED);
    CHECK(lpx_base(j->tag) == LPX_B_NEITHER && lpx_pol(j->tag) == LPX_P_NEGENTROPIC);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_ERR_DUP);
    cl = answer(liar, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_COUNTED);
    CHECK(lpx_base(j->tag) == LPX_B_BOTH && j->status == AIPI_JOB_PENDING);
    CHECK(lpx_pol(j->tag) == LPX_P_NEGENTROPIC && lpx_action(j->tag) == LPX_ACT_WAIT);
    CHECK(r->lg.escrow == 101);
    cl = answer(b, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_ACCEPTED);
    CHECK(j->status == AIPI_JOB_DONE && j->naccepted == 2 && j->nrejected == 1);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 13);
    /* 101 split over two agreeing executors: 51 + 50, liar 0, nothing lost */
    CHECK(lg_paid(&r->lg, a->a.self) == 51 && lg_paid(&r->lg, b->a.self) == 50);
    CHECK(lg_paid(&r->lg, liar->a.self) == 0);
    CHECK(r->lg.escrow == 0 && r->lg.balance == 899 && j->paid == 101);
    CHECK(j->slot[1].verdict == AIPI_V_REJECTED && j->slot[0].verdict == AIPI_V_ACCEPTED);
    CHECK(r->fx.runs == 0 && r->fx.checks == 0); /* nothing ran on the requester */
    aipi_release(&r->a, job);

    /* quorum reached by the first two: the third slot is never needed */
    CHECK(aipi_execute(&r->a, &f, u64in(77), 8, false, 3, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    cl = answer(a, r, claim, sizeof claim);
    aipi_submit(&r->a, job, claim, (uint32_t) cl, &v);
    cl = answer(b, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_ACCEPTED);
    CHECK(lg_paid(&r->lg, a->a.self) == 53 && lg_paid(&r->lg, b->a.self) == 51);
    aipi_release(&r->a, job);
    /* THE LIMIT, shown: two executors that collude on the same wrong answer
     * win a 2-of-3 quorum. Quorum assumes independent executors; choosing
     * them (agreements, trust, stake) is the host's job (docs/AIPI.md). */
    a->fx.lie = 5;
    b->fx.lie = 5;
    CHECK(aipi_execute(&r->a, &f, u64in(299), 8, false, 0, 0, 100, &job) == AIPI_OK);
    cl = answer(a, r, claim, sizeof claim);
    aipi_submit(&r->a, job, claim, (uint32_t) cl, &v);
    cl = answer(b, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_ACCEPTED);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 18);
    aipi_release(&r->a, job);
    node_free(r);
    node_free(a);
    node_free(b);
    node_free(liar);
}

static void test_quorum_dispute_and_expiry(void)
{
    node_t *r = node_new(1), *x = node_new(4), *y = node_new(5), *z = node_new(6);
    ipfsn_cid_t mod, f;
    aipi_fn_t fn;
    uint8_t claim[AIPI_CLAIM_ENC_MAX], v = 0;
    uint32_t job;
    int cl;
    const aipi_job_t *j;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    /* "salt" stands in for code that is not really deterministic: each
     * executor adds its own salt, so no two agree */
    make_fn(&fn, &mod, "salt", AIPI_VERIFY_QUORUM);
    CHECK(aipi_publish_fn(&r->a, &fn, &f) == AIPI_OK);
    publish(x, &fn, &f);
    publish(y, &fn, &f);
    publish(z, &fn, &f);
    x->fx.salt = 1;
    y->fx.salt = 2;
    z->fx.salt = 3;
    use_ledger(r, 100);

    CHECK(aipi_execute(&r->a, &f, u64in(10), 8, false, 30, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    cl = answer(x, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_COUNTED);
    cl = answer(y, r, claim, sizeof claim);
    /* 1 + 1 + one slot left can still make 2: keep waiting */
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_COUNTED);
    cl = answer(z, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_OK && v == AIPI_V_DISPUTED);
    CHECK(j->status == AIPI_JOB_DISPUTED);
    CHECK(lpx_base(j->tag) == LPX_B_BOTH && lpx_pol(j->tag) == LPX_P_ENTROPIC &&
          lpx_action(j->tag) == LPX_ACT_ESCALATE);
    CHECK(r->lg.balance == 100 && r->lg.escrow == 0 && r->lg.refunded == 30);
    CHECK(lg_paid(&r->lg, x->a.self) == 0 && lg_paid(&r->lg, z->a.self) == 0);
    CHECK(j->paid == 0 && j->refunded == 30);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_ERR_STATE);
    aipi_release(&r->a, job);

    /* expiry with no answers: refunded, NEITHER / entropic */
    CHECK(aipi_execute(&r->a, &f, u64in(11), 8, false, 40, 1000, 50, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    CHECK(r->lg.balance == 60);
    CHECK(aipi_tick(&r->a, 1049) == 0 && j->status == AIPI_JOB_PENDING);
    CHECK(aipi_tick(&r->a, 1050) == 1 && j->status == AIPI_JOB_EXPIRED);
    CHECK(lpx_base(j->tag) == LPX_B_NEITHER && lpx_pol(j->tag) == LPX_P_ENTROPIC);
    CHECK(r->lg.balance == 100 && r->lg.escrow == 0);
    CHECK(aipi_tick(&r->a, 5000) == 0); /* refunded once only */
    CHECK(r->lg.balance == 100);
    cl = answer(x, r, claim, sizeof claim);
    CHECK(aipi_submit(&r->a, job, claim, (uint32_t) cl, &v) == AIPI_ERR_STATE);
    aipi_release(&r->a, job);
    node_free(r);
    node_free(x);
    node_free(y);
    node_free(z);
}

static void test_privacy(void)
{
    node_t *r = node_new(1), *e = node_new(4);
    ipfsn_cid_t mod, f, fp;
    aipi_fn_t fn;
    uint8_t claim[AIPI_CLAIM_ENC_MAX], rec[AIPI_CALL_ENC_MAX], out[16];
    uint32_t job, len;
    const aipi_job_t *j;
    int rl;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&fn, &mod, "isqrt", AIPI_VERIFY_CHECKER);
    strcpy(fn.checker, "isqrt_ok");
    publish(e, &fn, &f);
    r->peer = e;
    use_ledger(r, 100);

    /* public function, private input: the code may be fetched, the call is
     * never broadcast and no bounty moves (A1) */
    CHECK(aipi_execute(&r->a, &f, u64in(144), 8, true, 20, 0, 100, &job) == AIPI_OK);
    j = aipi_job(&r->a, job);
    CHECK(j->route == AIPI_ROUTE_FETCH_LOCAL && j->status == AIPI_JOB_DONE);
    CHECK(r->nbroadcasts == 0 && r->lg.balance == 100);
    CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 12);
    aipi_release(&r->a, job);
    /* ...and its result is not memoized */
    CHECK(aipi_execute(&r->a, &f, u64in(144), 8, false, 20, 0, 100, &job) == AIPI_OK);
    CHECK(aipi_job(&r->a, job)->route == AIPI_ROUTE_MESH && r->nbroadcasts == 1);
    aipi_tick(&r->a, 1000);
    aipi_release(&r->a, job);

    /* a key-bearing function: descriptor sealed, never broadcast, never served */
    fn.privacy = AIPI_PRIV_LOCAL;
    CHECK(aipi_publish_fn(&r->a, &fn, &fp) == AIPI_OK);
    CHECK(ipfsn_is_private(&r->in, &fp));
    {
        int b = (int) r->nbroadcasts;
        CHECK(aipi_execute(&r->a, &fp, u64in(16), 8, false, 20, 0, 100, &job) == AIPI_OK);
        j = aipi_job(&r->a, job);
        CHECK(j->route == AIPI_ROUTE_LOCAL && (int) r->nbroadcasts == b);
        CHECK(aipi_result(&r->a, job, out, sizeof out, &len) == AIPI_OK && rd64(out) == 4);
        aipi_release(&r->a, job);
    }
    /* a peer cannot even resolve it, and an executor refuses to run it */
    e->peer = r;
    CHECK(aipi_execute(&e->a, &fp, u64in(16), 8, false, 0, 0, 100, &job) != AIPI_OK);
    {
        aipi_fn_t copy;
        ipfsn_cid_t fp2;
        CHECK(aipi_publish_fn(&e->a, &fn, &fp2) == AIPI_OK && ipfsn_cid_equal(&fp, &fp2));
        rl = aipi_call_encode(&fp, u64in(16), 8, rec, sizeof rec);
        CHECK(aipi_serve(&e->a, rec, (uint32_t) rl, claim, sizeof claim) == AIPI_ERR_PRIVATE);
        CHECK(aipi_load_fn(&e->a, &fp, &copy) == AIPI_OK && copy.privacy == AIPI_PRIV_LOCAL);
    }
    /* a LOCAL-verified function is not served for others either */
    {
        aipi_fn_t l;
        ipfsn_cid_t fl;
        make_fn(&l, &mod, "isqrt", AIPI_VERIFY_LOCAL);
        CHECK(aipi_publish_fn(&e->a, &l, &fl) == AIPI_OK);
        rl = aipi_call_encode(&fl, u64in(16), 8, rec, sizeof rec);
        CHECK(aipi_serve(&e->a, rec, (uint32_t) rl, claim, sizeof claim) == AIPI_ERR_PRIVATE);
    }
    /* offline with no cached code: private work fails, it does not leak */
    {
        node_t *c = node_new(7);
        c->peer = e;
        c->a.online = false;
        CHECK(aipi_publish_fn(&c->a, &fn, &fp) == AIPI_OK);
        CHECK(aipi_execute(&c->a, &fp, u64in(16), 8, false, 0, 0, 100, &job) == AIPI_ERR_OFFLINE);
        CHECK(c->wants == 0 && c->nbroadcasts == 0);
        node_free(c);
    }
    node_free(r);
    node_free(e);
}

static void test_codec_and_funds(void)
{
    node_t *r = node_new(1);
    ipfsn_cid_t mod, f, wrong, data;
    aipi_fn_t fn, back;
    uint8_t enc[AIPI_FN_ENC_MAX];
    uint32_t job;
    int len;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&fn, &mod, "smallest_factor", AIPI_VERIFY_QUORUM);
    publish(r, &fn, &f);
    /* the same descriptor bytes stored under another intent's codec */
    len = aipi_fn_encode(&fn, enc, sizeof enc);
    CHECK(ipfsn_cid_sha256(AIPI_MC_SETTLE_LEDGER, enc, (uint32_t) len, &wrong) == IPFSN_OK);
    CHECK(ipfsn_bs_put(&r->bs, &wrong, enc, (uint32_t) len, IPFSN_VIS_PUBLIC, 0) == IPFSN_OK);
    CHECK(aipi_load_fn(&r->a, &wrong, &back) == AIPI_ERR_CODEC);
    CHECK(aipi_execute(&r->a, &wrong, u64in(1), 8, false, 0, 0, 1, &job) == AIPI_ERR_CODEC);
    /* a data CID is not a function */
    CHECK(aipi_put_blob(&r->a, enc, (uint32_t) len, IPFSN_VIS_PUBLIC, &data) == AIPI_OK);
    CHECK(aipi_load_fn(&r->a, &data, &back) == AIPI_ERR_UNSUPP);
    /* a block under a function codec that is not a descriptor */
    {
        ipfsn_cid_t junk;
        ipfsn_cid_sha256(AIPI_MC_FN, MODULE, sizeof MODULE, &junk);
        ipfsn_bs_put(&r->bs, &junk, MODULE, sizeof MODULE, IPFSN_VIS_PUBLIC, 0);
        CHECK(aipi_load_fn(&r->a, &junk, &back) == AIPI_ERR_MALFORMED);
    }

    /* the bounty cannot be reserved: nothing is broadcast */
    use_ledger(r, 5);
    r->a.prefer_local = false;
    {
        node_t *q = node_new(8);
        use_ledger(q, 5);
        CHECK(aipi_publish_fn(&q->a, &fn, &f) == AIPI_OK);
        CHECK(aipi_execute(&q->a, &f, u64in(1), 8, false, 6, 0, 1, &job) == AIPI_ERR_FUNDS);
        CHECK(q->nbroadcasts == 0 && q->lg.balance == 5 && aipi_job(&q->a, 0) == NULL);
        q->lg.fail_reserve = 1;
        CHECK(aipi_execute(&q->a, &f, u64in(1), 8, false, 1, 0, 1, &job) == AIPI_ERR_FUNDS);
        q->lg.fail_reserve = 0;
        /* a bounty with no economy bound is an argument error, not free work */
        q->a.mesh.reserve = NULL;
        CHECK(aipi_execute(&q->a, &f, u64in(1), 8, false, 1, 0, 1, &job) == AIPI_ERR_ARG);
        /* zero bounty needs no economy */
        CHECK(aipi_execute(&q->a, &f, u64in(1), 8, false, 0, 0, 1, &job) == AIPI_OK);
        CHECK(q->nbroadcasts == 1 && q->last_bounty == 0);
        node_free(q);
    }
    CHECK(aipi_init(NULL, &r->in, r->jobs, JOBS, 0, 0, r->modbuf, r->conbuf) == AIPI_ERR_ARG);
    CHECK(aipi_init(&r->a, &r->in, r->jobs, 0, 0, 0, r->modbuf, r->conbuf) == AIPI_ERR_ARG);
    CHECK(!strcmp(aipi_status_name(AIPI_ERR_CODEC), "AIPI_ERR_CODEC"));
    node_free(r);
}

/* Random mixes of honest, lying and silent executors: whatever happens, the
 * escrow balances (A2) and only verified executors are paid. */
static void test_escrow_invariant(void)
{
    node_t *r = node_new(1), *ex[4];
    ipfsn_cid_t mod, fq, fc;
    aipi_fn_t q, c;
    uint32_t seed = 12345, total = 10000;
    uint8_t claim[AIPI_CLAIM_ENC_MAX], v;
    int bad = 0;

    ipfsn_cid_sha256(IPFSN_MC_RAW, MODULE, sizeof MODULE, &mod);
    make_fn(&q, &mod, "smallest_factor", AIPI_VERIFY_QUORUM);
    make_fn(&c, &mod, "isqrt", AIPI_VERIFY_CHECKER);
    strcpy(c.checker, "isqrt_ok");
    publish(r, &q, &fq);
    publish(r, &c, &fc);
    for (int i = 0; i < 4; i++) {
        ex[i] = node_new((uint8_t) (20 + i));
        publish(ex[i], &q, &fq);
        publish(ex[i], &c, &fc);
    }
    use_ledger(r, total);
    for (int round = 0; round < 200; round++) {
        uint32_t job;
        uint64_t bounty, n;
        const aipi_job_t *j;
        seed = seed * 1103515245u + 12345u;
        bounty = (seed >> 8) % 37;
        n = 2 + (seed >> 4) % 5000;
        if (aipi_execute(&r->a, round & 1 ? &fq : &fc, u64in(n), 8, false, bounty, 0, 10, &job) !=
            AIPI_OK) {
            bad++;
            continue;
        }
        j = aipi_job(&r->a, job);
        for (int i = 0; i < 4 && j->status == AIPI_JOB_PENDING; i++) {
            int cl;
            seed = seed * 1103515245u + 12345u;
            ex[i]->fx.lie = (seed >> 16) % 3 == 0 ? i + 1 : 0; /* liars disagree */
            if ((seed >> 20) % 4 == 0) continue;               /* silent */
            cl = answer(ex[i], r, claim, sizeof claim);
            aipi_submit(&r->a, job, claim, (uint32_t) cl, &v);
            if (v == AIPI_V_ACCEPTED && j->slot[j->nclaims - 1].verdict != AIPI_V_ACCEPTED) bad++;
        }
        aipi_tick(&r->a, 10);
        if (j->status == AIPI_JOB_PENDING) bad++;
        if (j->paid + j->refunded != j->bounty) bad++;
        if (j->status == AIPI_JOB_DONE) {
            /* the stored result is the true one */
            uint64_t want = round & 1 ? least_factor(n) : isqrt(n);
            if (rd64(j->out) != want) bad++;
            if (j->route == AIPI_ROUTE_MESH && j->refunded) bad++;
        } else if (j->paid) {
            bad++;
        }
        for (uint32_t s = 0; s < j->nclaims; s++)
            if (j->slot[s].verdict == AIPI_V_ACCEPTED &&
                rd64(j->slot[s].out) != (round & 1 ? least_factor(n) : isqrt(n)))
                bad++;
        aipi_release(&r->a, job);
    }
    {
        uint64_t paid = 0;
        for (int i = 0; i < r->lg.nids; i++) paid += r->lg.paid_to[i];
        CHECK(r->lg.escrow == 0);
        CHECK(r->lg.balance + paid == total);
    }
    CHECK(bad == 0);
    node_free(r);
    for (int i = 0; i < 4; i++) node_free(ex[i]);
}

int main(void)
{
    test_cids_and_text();
    test_descriptor_rules();
    test_records();
    test_get();
    test_local_offline_and_fetch();
    test_mesh_checker();
    test_mesh_cert();
    test_mesh_quorum();
    test_quorum_dispute_and_expiry();
    test_privacy();
    test_codec_and_funds();
    test_escrow_invariant();
    printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
