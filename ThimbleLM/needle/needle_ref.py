"""Needle 3 in numpy: a line-by-line translation of cactus/model/architecture.py (JAX) run on
the shipped needle3.cact (CQ weights dequantized exactly as the exporter's _cq_unpack does),
with the deployment numerics the archive declares: A8 activations, A8 queries, int8 KV.

It is the reference the C engine (engine/src/needle.c) is checked against, and it runs the
Cactus frozen suites (suites.json) to show the port is right. Positions are processed in
chunks against a cache, so a whole prompt and a single decode step are the same code path.
"""
import json
import math
import struct
import sys

import numpy as np

TAG = 0x05E12A84
HDR = "<48If"
REC = "<BBHIIIIQQII"
FP16, FP32, CQ, RAW = 1, 2, 3, 4
SP = "▁"
# chat markers (cactus/model/tokenizer.py)
BOS, EOS, PAD = 2, 1, 0


def walsh(n):
    H = np.array([[1.0]], np.float32)
    while H.shape[0] < n:
        H = np.block([[H, H], [H, -H]])
    return (H / np.sqrt(n)).astype(np.float32)


class Cact:
    def __init__(self, path):
        raw = open(path, "rb").read()
        self.raw = raw
        h = struct.unpack_from(HDR, raw, 0)
        assert h[0] == TAG, "not a Needle 3 archive"
        names = ("tag", "num_tensors", "cb_n", "kv_window", "kv_bits", "vocab", "out_vocab", "d_model",
                 "num_heads", "num_kv_heads", "num_layers", "qk_hd", "v_hd", "max_seq", "hada_n",
                 "lanes", "sliding_window", "gmask_lo", "gmask_hi", "conv_taps", "eg_slots", "eg_sub",
                 "eg_tables", "eg_taps", "eg_dilation", "eg_seed_heads", "n_orders")
        self.h = dict(zip(names, h))
        self.orders = h[27:27 + h[26]]
        self.sites = h[32:32 + h[31]]
        self.rope_theta = h[48]
        gm = h[17] | (h[18] << 32)
        self.global_layers = {i for i in range(h[10]) if gm >> i & 1}
        off = struct.calcsize(HDR)
        cb = np.frombuffer(raw, np.float32, h[2], off).copy()
        self.cb = {2: cb[0:4], 3: cb[4:12], 4: cb[12:28]}
        off += h[2] * 4
        rs = struct.calcsize(REC)
        self.recs = []
        for i in range(h[1]):
            r = struct.unpack_from(REC, raw, off + i * rs)
            self.recs.append(dict(dtype=r[0], shape=tuple(r[3:3 + r[1]]), off=r[7], n=r[8], group=r[9], bits=r[10]))
        self.H = {}

    def blob(self, i):
        r = self.recs[i]
        return self.raw[r["off"]:r["off"] + r["n"]]

    def get(self, i, rows=None):
        """tensor i as float32 (CQ dequantized: (cb[idx] * norm) @ H per group); rows= picks rows"""
        r = self.recs[i]
        if r["dtype"] == FP16:
            return np.frombuffer(self.blob(i), np.float16).reshape(r["shape"]).astype(np.float32)
        if r["dtype"] == FP32:
            return np.frombuffer(self.blob(i), np.float32).reshape(r["shape"]).copy()
        if r["dtype"] == RAW:
            return self.blob(i)
        out, din = r["shape"]
        g, bits = r["group"], r["bits"]
        pad = (din + g - 1) // g * g
        rowb = pad * bits // 8
        b = self.blob(i)
        packed = np.frombuffer(b, np.uint8, out * rowb).reshape(out, rowb)
        norms = np.frombuffer(b, np.float16, out * (pad // g), out * rowb).reshape(out, pad // g)
        if rows is not None:
            packed, norms = packed[rows], norms[rows]
        n = packed.shape[0]
        # LSB-first bitstream per row
        bitsarr = np.unpackbits(packed, axis=1, bitorder="little").reshape(n, pad, bits)
        idx = (bitsarr * (1 << np.arange(bits, dtype=np.uint8))).sum(-1)
        if g not in self.H:
            self.H[g] = walsh(g)
        unit = self.cb[bits][idx].reshape(n, pad // g, g)
        w = (unit * norms.astype(np.float32)[:, :, None]) @ self.H[g]
        return w.reshape(n, pad)[:, :din]


class Tokenizer:
    """cactus/model/export.py RefTokenizer over the archive's RAW tokenizer blob"""

    def __init__(self, blob):
        n, pad, eos, bos, unk, dummy, bytefb, _ = struct.unpack_from("<IIIIIBBH", blob, 0)
        off = struct.calcsize("<IIIIIBBH")
        self.pieces, self.scores, self.types = [], [], []
        for _ in range(n):
            s, t, ln = struct.unpack_from("<fBH", blob, off)
            off += struct.calcsize("<fBH")
            self.pieces.append(blob[off:off + ln].decode("utf-8"))
            self.scores.append(s)
            self.types.append(t)
            off += ln
        self.add_dummy, self.byte_fallback, self.unk = bool(dummy), bool(bytefb), unk
        self.p2id = {p: i for i, p in enumerate(self.pieces)}
        self.byte_id = {int(p[3:5], 16): i for i, (p, t) in enumerate(zip(self.pieces, self.types)) if t == 4}
        self.markers = sorted((p for p, t in zip(self.pieces, self.types) if t == 3), key=len, reverse=True)

    def _bpe(self, seg):
        syms = list(seg)
        while len(syms) > 1:
            best, bj = None, -1
            for j in range(len(syms) - 1):
                i = self.p2id.get(syms[j] + syms[j + 1])
                if i is not None and (best is None or self.scores[i] > best):
                    best, bj = self.scores[i], j
            if bj < 0:
                break
            syms[bj:bj + 2] = [syms[bj] + syms[bj + 1]]
        ids = []
        for s in syms:
            i = self.p2id.get(s)
            if i is not None:
                ids.append(i)
            elif self.byte_fallback:
                ids.extend(self.byte_id[b] for b in s.encode("utf-8"))
            else:
                ids.append(self.unk)
        return ids

    def encode(self, text):
        if not text:
            return []
        esc = text.replace(" ", SP)
        if self.add_dummy:
            esc = SP + esc
        ids, buf, i = [], [], 0
        while i < len(esc):
            m = next((m for m in self.markers if esc.startswith(m, i)), None)
            if m is not None:
                ids += self._bpe("".join(buf))
                buf = []
                ids.append(self.p2id[m])
                i += len(m)
            else:
                buf.append(esc[i])
                i += 1
        return ids + self._bpe("".join(buf))

    def token_bytes(self, i):
        """what token i adds to the output text (markers and controls: None)"""
        t = self.types[i]
        if t == 4:
            return bytes([int(self.pieces[i][3:5], 16)])
        if t in (1, 2, 3):
            return None
        return self.pieces[i].replace(SP, " ").encode("utf-8")


def aq(x):
    """fake_quant(x, x.shape[-1], 8): symmetric per-row int8, round half to even"""
    a = np.max(np.abs(x), axis=-1, keepdims=True)
    s = np.where(a > 0, a / 127.0, 1.0)
    return np.clip(np.round(x / s), -128, 127) * s


def zc_norm(x, scale, eps=1e-6):
    return (1 + scale) * x / np.sqrt(np.mean(x * x, axis=-1, keepdims=True) + eps)


def rms_unit(x, eps=1e-6):
    return x / np.sqrt(np.mean(x * x, axis=-1, keepdims=True) + eps)


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def silu(x):
    return x * sigmoid(x)


def softmax(x, axis=-1):
    m = np.max(x, axis=axis, keepdims=True)
    e = np.exp(x - m)
    return e / np.sum(e, axis=axis, keepdims=True)


def logsumexp(x, axis):
    m = np.max(x, axis=axis, keepdims=True)
    return m + np.log(np.sum(np.exp(x - m), axis=axis, keepdims=True))


def sinkhorn(logits, iters=20):
    lk = logits
    for _ in range(iters):
        lk = lk - logsumexp(lk, -1)
        lk = lk - logsumexp(lk, -2)
    return np.exp(lk)


SEED, PRIME = 0x9E3779B9, 0x01000193


class Model:
    def __init__(self, path, quant=True, local_window=None):
        c = self.c = Cact(path)
        h = c.h
        self.quant = quant
        self.D, self.NH, self.NKV, self.L = h["d_model"], h["num_heads"], h["num_kv_heads"], h["num_layers"]
        self.QK, self.V = h["qk_hd"], h["v_hd"]
        self.lanes, self.taps = h["lanes"], h["conv_taps"]
        self.window = local_window if local_window is not None else h["sliding_window"]
        self.tok = Tokenizer(c.get(len(c.recs) - 1))
        self.emb = c.get(0)                                         # (8192, 768), also the head
        L, ti = self.L, 1
        self.layers = []
        for i in range(L):
            g = lambda k: c.get(ti + k)
            names = ["norm_in", "q", "k", "v", "q_taps", "k_taps", "v_taps", "q_norm", "k_norm", "gate", "out",
                     "post_norm", "attn_gate", "pre_hada", "d1", "d2", "b2", "d3", "d4", "w1a", "w1b", "w2a",
                     "w2b", "w3a", "w3b", "cond_v", "cond_u"]
            self.layers.append({n: g(k) for k, n in enumerate(names)})
            ti += len(names)
        self.a_pre, self.a_post, self.a_res = c.get(ti), c.get(ti + 1), c.get(ti + 2)
        self.b_pre, self.b_post, self.b_res = c.get(ti + 3), c.get(ti + 4), c.get(ti + 5)
        n = self.lanes
        self.phi_pre = c.get(ti + 6).reshape(L, n, -1)               # rows l*lanes+lane = phi[l,:,lane]
        self.phi_post = c.get(ti + 7).reshape(L, n, -1)
        self.phi_res = c.get(ti + 8).reshape(L, n * n, -1)
        self.p1 = c.get(ti + 9).astype(np.int64)
        self.p2 = c.get(ti + 10).astype(np.int64)
        ti += 11
        self.engram = []
        for s in range(len(c.sites)):
            self.engram.append({"tables": ti, "key": c.get(ti + 1), "value": c.get(ti + 2), "taps": c.get(ti + 3)})
            ti += 4
        self.final_norm = c.get(ti)
        ti += 1
        self.conf = None
        if ti < len(c.recs) - 1:
            manifest = c.get(ti)
            ti += 1
            if 2 in [int(round(x)) for x in manifest]:
                k = 4
                self.conf = {"probes": c.get(ti).reshape(L + 1, k, self.D), "gain": c.get(ti + 1),
                             "query": c.get(ti + 2), "row_bias": c.get(ti + 3), "proj": c.get(ti + 4)[0],
                             "bias": float(c.get(ti + 5)[0])}
        self.orders = c.orders
        self.heads_per_order = h["eg_tables"] // len(self.orders)
        self.slots = h["eg_slots"]
        self.dil = h["eg_dilation"]
        freqs = 1.0 / (self.rope_theta() ** (np.arange(0, self.QK, 2, dtype=np.float32) / self.QK))
        self.freqs = freqs.astype(np.float32)
        self.H32 = None

    def rope_theta(self):
        return self.c.rope_theta

    # ---- per-sequence state ----
    def new_state(self):
        return {"tokens": [], "K": [np.zeros((0, self.NKV, self.QK), np.float32) for _ in range(self.L)],
                "Vc": [np.zeros((0, self.NKV, self.V), np.float32) for _ in range(self.L)],
                "raw": [[np.zeros((0, self.NH * self.QK), np.float32), np.zeros((0, self.NKV * self.QK), np.float32),
                         np.zeros((0, self.NKV * self.V), np.float32)] for _ in range(self.L)],
                "ev_raw": [np.zeros((0, self.D), np.float32) for _ in self.engram],
                "cells": []}

    def engram_rows(self, toks, t):
        """table rows for position t: for each order and head, a hash of the last `order` tokens"""
        idx = []
        for oi, order in enumerate(self.orders):
            for hh in range(self.heads_per_order):
                seed = (SEED * (oi * self.heads_per_order + hh + 1)) & 0xFFFFFFFF
                acc = seed
                for j in range(order):
                    u = toks[t - j] if t - j >= 0 else 0
                    acc = ((acc ^ u) * PRIME) & 0xFFFFFFFF
                acc ^= acc >> 15
                idx.append((acc % self.slots, order))
        return idx

    def engram_kv(self, st, s, t0, t1):
        eg = self.engram[s]
        toks = st["tokens"]
        rows, ok = [], []
        for t in range(t0, t1):
            for ti_, (slot, order) in enumerate(self.engram_rows(toks, t)):
                rows.append(ti_ * self.slots + slot)
                ok.append(1.0 if t >= order - 1 else 0.0)
        fetched = self.c.get(eg["tables"], rows=np.array(rows)) * np.array(ok, np.float32)[:, None]
        e = fetched.reshape(t1 - t0, -1)
        if self.quant:
            e = aq(e)
        k = e @ eg["key"].T
        vraw = e @ eg["value"].T
        st["ev_raw"][s] = np.concatenate([st["ev_raw"][s], vraw])
        allv = st["ev_raw"][s]
        v = np.zeros_like(vraw)
        for j in range(eg["taps"].shape[0]):
            for t in range(t0, t1):
                src = t - j * self.dil
                if src >= 0:
                    v[t - t0] += eg["taps"][j] * allv[src]
        return k, v

    def rope(self, x, pos):
        ang = pos[:, None].astype(np.float32) * self.freqs[None, :]
        cos, sin = np.cos(ang), np.sin(ang)
        half = x.shape[-1] // 2
        x1, x2 = x[..., :half], x[..., half:]
        cos, sin = cos[:, None, :], sin[:, None, :]
        return np.concatenate([x1 * cos - x2 * sin, x2 * cos + x1 * sin], axis=-1)

    def kron(self, z, a, b):
        Z = z.reshape(z.shape[0], a.shape[0], b.shape[0])
        return np.einsum("tij,ik,jl->tkl", Z, a, b).reshape(z.shape[0], -1)

    def hada(self, ly, x):
        n = 1024
        cond = 1 + softmax(x @ ly["cond_v"], -1) @ ly["cond_u"]
        z = np.zeros((x.shape[0], n), np.float32)
        z[:, :x.shape[1]] = x
        z = self.kron(ly["d1"] * z, ly["w1a"], ly["w1b"])[:, self.p1]
        z = self.kron(silu(ly["d2"] * cond * z + ly["b2"]), ly["w2a"], ly["w2b"])[:, self.p2]
        z = self.kron(ly["d3"] * z, ly["w3a"], ly["w3b"])
        return (ly["d4"] * z)[:, :self.D]

    def attention(self, st, li, ly, x, t0, t1):
        T = t1 - t0
        xq = aq(x) if self.quant else x
        raw_new = [xq @ ly["q"].T, xq @ ly["k"].T, xq @ ly["v"].T]
        outs = []
        for which, taps in ((0, ly["q_taps"]), (1, ly["k_taps"]), (2, ly["v_taps"])):
            st["raw"][li][which] = np.concatenate([st["raw"][li][which], raw_new[which]])
            allr = st["raw"][li][which]
            z = np.zeros_like(raw_new[which])
            for j in range(taps.shape[0]):
                for t in range(t0, t1):
                    if t - j >= 0:
                        z[t - t0] += taps[j] * allr[t - j]
            outs.append(z)
        q = outs[0].reshape(T, self.NH, self.QK)
        k = outs[1].reshape(T, self.NKV, self.QK)
        v = outs[2].reshape(T, self.NKV, self.V)
        q = zc_norm(q, ly["q_norm"])
        k = zc_norm(k, ly["k_norm"])
        pos = np.arange(t0, t1)
        q, k = self.rope(q, pos), self.rope(k, pos)
        if self.quant:
            q, k, v = aq(q), aq(k), aq(v)
        st["K"][li] = np.concatenate([st["K"][li], k])
        st["Vc"][li] = np.concatenate([st["Vc"][li], v])
        K, Vc = st["K"][li], st["Vc"][li]
        rep = self.NH // self.NKV
        Kh = np.repeat(K, rep, axis=1)                                    # (S, NH, QK)
        Vh = np.repeat(Vc, rep, axis=1)
        sc = np.einsum("thd,shd->hts", q, Kh) / math.sqrt(self.QK)
        S = K.shape[0]
        tpos = pos[:, None]
        spos = np.arange(S)[None, :]
        mask = spos <= tpos
        if li not in self.c.global_layers and self.window:
            mask &= (tpos - spos) < self.window
        sc = np.where(mask[None], sc, -np.inf)
        p = softmax(sc, -1)
        out = np.einsum("hts,shd->thd", p, Vh).reshape(T, self.NH * self.V)
        out = out * sigmoid(xq @ ly["gate"].T)
        if self.quant:
            out = aq(out)
        return out @ ly["out"].T

    def block(self, st, li, u, ekv, t0, t1):
        ly = self.layers[li]
        x = u
        if ekv is not None:
            ek, ev = ekv
            alpha = sigmoid(np.sum(rms_unit(x) * rms_unit(ek), -1, keepdims=True) / math.sqrt(self.D))
            x = x + alpha * ev
        skip = x
        hh = zc_norm(x, ly["norm_in"])
        hh = self.attention(st, li, ly, hh, t0, t1)
        hh = zc_norm(hh, ly["post_norm"])
        x = skip + sigmoid(ly["attn_gate"][0]) * hh
        skip = x
        hh = zc_norm(x, ly["pre_hada"])
        return skip + self.hada(ly, hh)

    def feed(self, st, new_tokens):
        """runs new tokens through the model; returns logits for each"""
        t0 = len(st["tokens"])
        st["tokens"] = st["tokens"] + list(new_tokens)
        t1 = len(st["tokens"])
        T, n, D = t1 - t0, self.lanes, self.D
        x0 = self.emb[np.array(new_tokens)] * math.sqrt(D)
        ekv = {}
        for s, layer in enumerate(self.c.sites):
            ekv[layer] = self.engram_kv(st, s, t0, t1)
        X = np.repeat(x0[:, None, :], n, axis=1)                         # (T, lanes, D)
        cells = [x0]
        for li in range(self.L):
            lane = np.eye(n, dtype=np.float32)[li % n]
            pre_off, post_off = 8 * lane - 4, -4 * (1 - lane)
            nx = rms_unit(X.reshape(T, n * D))
            if self.quant:
                nx = aq(nx)
            hpre = sigmoid(self.a_pre[li] * (nx @ self.phi_pre[li].T) + self.b_pre[li] + pre_off)
            u = np.einsum("tn,tnc->tc", hpre, X)
            y = self.block(st, li, u, ekv.get(li), t0, t1) - u
            hpost = 2 * sigmoid(self.a_post[li] * (nx @ self.phi_post[li].T) + self.b_post[li] + post_off)
            res = (nx @ self.phi_res[li].T).reshape(T, n, n)
            hres = sinkhorn(self.a_res[li] * res + self.b_res[li])
            X = np.einsum("tij,tjc->tic", hres, X) + hpost[..., None] * y[:, None, :]
            cells.append(X.mean(1))
        st["cells"].append(np.stack(cells, axis=1))                       # (T, L+1, D)
        x = zc_norm(X.mean(1), self.final_norm)
        if self.quant:
            x = aq(x)
        return x @ self.emb.T

    def confidence(self, st):
        """the calibrated head over every position so far (probe_pool + proj); a probability"""
        if self.conf is None:
            return None
        cf = np.concatenate(st["cells"])                                  # (T, L+1, D)
        c = self.conf
        d = self.D
        scores = np.einsum("tld,lkd->lkt", cf, c["probes"]) / math.sqrt(d)
        r = np.einsum("lkt,tld->lkd", softmax(scores, -1), cf)
        r = rms_unit(r) * c["gain"][:, :, None]
        u = np.einsum("lkd,qd->qlk", r, c["query"]) / math.sqrt(d) + c["row_bias"]
        Lq = u.shape[0]
        w = softmax(u.reshape(Lq, -1), -1)
        pooled = (w @ r.reshape(-1, d)).reshape(-1)
        return float(sigmoid(pooled @ c["proj"] + c["bias"]))

    def copy_state(self, st):
        return {"tokens": list(st["tokens"]), "K": [k.copy() for k in st["K"]], "Vc": [v.copy() for v in st["Vc"]],
                "raw": [[r.copy() for r in rr] for rr in st["raw"]], "ev_raw": [e.copy() for e in st["ev_raw"]],
                "cells": list(st["cells"])}


IM_START, IM_END = "<|im_start|>", "<|im_end|>"


def render_prefix(tools, system=""):
    tools_json = json.dumps(tools, separators=(",", ":"), ensure_ascii=False)
    pre = IM_START + "system\n" + system + IM_END + "\n" if system else ""
    return pre + IM_START + "user\n" + "<tools>" + tools_json + "</tools>"


def render_query(query):
    return "\n" + query + IM_END + "\n" + IM_START + "assistant\n"


if __name__ == "__main__":
    m = Model(sys.argv[1])
    tools = [{"name": "set_volume", "description": "Set the speaker volume.",
              "parameters": {"type": "object", "properties": {"level": {"type": "integer", "minimum": 0, "maximum": 100}},
                             "required": ["level"]}}]
    prompt = render_prefix(tools) + render_query("set the volume to 40")
    ids = [BOS] + m.tok.encode(prompt)
    print(len(ids), "prompt tokens")
    st = m.new_state()
    logits = m.feed(st, ids)[-1]
    out = []
    for _ in range(60):
        t = int(np.argmax(logits))
        if t == EOS:
            break
        out.append(t)
        logits = m.feed(st, [t])[-1]
    print(repr("".join(m.tok.pieces[t] for t in out).replace(SP, " ")))
    print("confidence head", m.confidence(st))
