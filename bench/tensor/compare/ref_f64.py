#!/usr/bin/env python3
# Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC
# SPDX-License-Identifier: Apache-2.0
# Float64 reference forward pass of a qwen2 GGUF (llama.cpp semantics), for N token ids.
# Weights are dequantized exactly from the file (gguf-py), all arithmetic in float64.
# usage: ref_f64.py MODEL IDS.bin N OUT.f32
import sys, numpy as np
from gguf import GGUFReader
from gguf.quants import dequantize
M, IDS, N, OUT = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
r = GGUFReader(M)
def kv(k):
    f = r.fields[k]; return f.parts[f.data[0]][0]
T = {t.name: t for t in r.tensors}
def W(name):
    t = T[name]
    a = dequantize(t.data, t.tensor_type).astype(np.float64)
    return a.reshape([int(x) for x in reversed(t.shape)]) if a.ndim == 1 or a.size == int(np.prod(t.shape)) else a
nl = int(kv('qwen2.block_count')); ne = int(kv('qwen2.embedding_length'))
nh = int(kv('qwen2.attention.head_count')); nkv = int(kv('qwen2.attention.head_count_kv'))
eps = float(kv('qwen2.attention.layer_norm_rms_epsilon')); base = float(kv('qwen2.rope.freq_base'))
hd = ne // nh
ids = np.fromfile(IDS, dtype=np.int32)[:N]
emb = W('token_embd.weight')
x = emb[ids].copy()
def rms(v, g): return v / np.sqrt((v * v).mean(-1, keepdims=True) + eps) * g
pos = np.arange(N)
inv = base ** (-np.arange(0, hd, 2) / hd)
ang = pos[:, None] * inv[None, :]
cos, sin = np.cos(ang), np.sin(ang)
def rope(q):  # q: [N, h, hd], NEOX halves
    a, b = q[..., :hd // 2], q[..., hd // 2:]
    c, s = cos[:, None, :], sin[:, None, :]
    return np.concatenate([a * c - b * s, a * s + b * c], -1)
maxabs = []
for l in range(nl):
    p = f'blk.{l}.'
    h = rms(x, W(p + 'attn_norm.weight'))
    q = h @ W(p + 'attn_q.weight').T + W(p + 'attn_q.bias')
    k = h @ W(p + 'attn_k.weight').T + W(p + 'attn_k.bias')
    v = h @ W(p + 'attn_v.weight').T + W(p + 'attn_v.bias')
    q = rope(q.reshape(N, nh, hd)); k = rope(k.reshape(N, nkv, hd)); v = v.reshape(N, nkv, hd)
    out = np.zeros((N, nh, hd))
    mask = np.triu(np.full((N, N), -np.inf), 1)
    for hh in range(nh):
        g = hh // (nh // nkv)
        s = q[:, hh] @ k[:, g].T / np.sqrt(hd) + mask
        s = np.exp(s - s.max(1, keepdims=True)); s /= s.sum(1, keepdims=True)
        out[:, hh] = s @ v[:, g]
    x = x + out.reshape(N, -1) @ W(p + 'attn_output.weight').T
    h = rms(x, W(p + 'ffn_norm.weight'))
    gt = h @ W(p + 'ffn_gate.weight').T; up = h @ W(p + 'ffn_up.weight').T
    a = gt / (1 + np.exp(-gt)) * up
    maxabs.append((l, float(np.abs(a).max()), float(np.abs(x).max())))
    x = x + a @ W(p + 'ffn_down.weight').T
    print(f'layer {l}: max|silu*up| {maxabs[-1][1]:.1f} max|x before ffn| {maxabs[-1][2]:.1f} max|x after| {np.abs(x).max():.1f}', flush=True)
h = rms(x, W('output_norm.weight'))
out_w = W('output.weight') if 'output.weight' in T else emb
logits = h @ out_w.T
logits.astype(np.float32).tofile(OUT)
print('done', logits.shape)
