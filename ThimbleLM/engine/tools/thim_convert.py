#!/usr/bin/env python3
"""Turns Whisper (tiny.en layout) weights from Hugging Face into a .thim file for the Thimble engine.

A .thim file is not a general model format. It holds exactly what the engine's speech-to-text
loop reads, laid out the way that loop reads it, so loading is a pointer cast and nothing is
converted at run time:

  * every matrix is int8 in blocks of 32 along the input dimension (a float scale per block),
    padded to a whole number of blocks, rows contiguous: one output row is one linear read;
  * convolutions are stored as matrices over the unrolled kernel window ([out][tap][in]) so
    they run as the same matrix kernel as everything else;
  * the mel filter bank, the Hann window, the sinusoidal and learned position tables and the
    token table (the bytes of each token) are inside the file, so the engine needs no other file;
  * every block starts on a 64-byte boundary.

Usage: thim_convert.py HF_DIR OUT.thim [--check]
"""
import json, struct, sys, os
import numpy as np

BLOCK = 32
MAGIC = b"THIM"
VERSION = 1
KIND_WHISPER = 1

def bytes_to_unicode():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + list(range(ord("®"), ord("ÿ") + 1))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b); cs.append(256 + n); n += 1
    return dict(zip(bs, [chr(c) for c in cs]))

class Writer:
    def __init__(self):
        self.buf = bytearray(256)       # the header goes here at the end; every offset is from the file start
        self.table = []          # (name, kind, rows, cols, cols_pad, data_off, scale_off)

    def align(self, a=64):
        while len(self.buf) % a:
            self.buf.append(0)

    def put(self, raw):
        self.align()
        off = len(self.buf)
        self.buf += raw
        return off

    def f32(self, name, arr):
        a = np.ascontiguousarray(arr, dtype="<f4")
        rows = a.shape[0] if a.ndim > 1 else 1
        cols = a.size // rows
        off = self.put(a.tobytes())
        self.table.append((name, 0, rows, cols, cols, off, 0))

    def q8(self, name, mat):
        """mat: [rows][cols] float -> int8 blocks of 32 with float scales"""
        m = np.asarray(mat, dtype=np.float32)
        rows, cols = m.shape
        pad = (-cols) % BLOCK
        if pad:
            m = np.concatenate([m, np.zeros((rows, pad), np.float32)], axis=1)
        cp = m.shape[1]
        blocks = m.reshape(rows, cp // BLOCK, BLOCK)
        amax = np.abs(blocks).max(axis=2)
        scale = np.where(amax > 0, amax / 127.0, 1.0).astype(np.float32)
        q = np.clip(np.rint(blocks / scale[:, :, None]), -127, 127).astype(np.int8)
        doff = self.put(q.reshape(rows, cp).tobytes())
        soff = self.put(np.ascontiguousarray(scale, dtype="<f4").tobytes())
        self.table.append((name, 1, rows, cols, cp, doff, soff))


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    hf, out = sys.argv[1], sys.argv[2]
    from safetensors.numpy import load_file
    W = load_file(os.path.join(hf, "model.safetensors"))
    cfg = json.load(open(os.path.join(hf, "config.json")))
    pre = json.load(open(os.path.join(hf, "preprocessor_config.json")))
    d = cfg["d_model"]; nh = cfg["encoder_attention_heads"]; ne = cfg["encoder_layers"]; nd = cfg["decoder_layers"]
    ffn = cfg["encoder_ffn_dim"]; nm = cfg["num_mel_bins"]; vocab = cfg["vocab_size"]
    n_audio = cfg["max_source_positions"]; n_text = cfg["max_target_positions"]
    assert cfg["decoder_ffn_dim"] == ffn and cfg["decoder_attention_heads"] == nh

    w = Writer()
    # --- front end tables
    mel = np.array(pre["mel_filters"], dtype=np.float32)
    if mel.shape == (201, nm): mel = mel.T
    assert mel.shape == (nm, 201), mel.shape
    w.f32("mel", mel.copy())                                        # [80][201]: one filter per row
    n = np.arange(400)
    w.f32("window", 0.5 - 0.5 * np.cos(2 * np.pi * n / 400))        # periodic Hann (torch.hann_window)

    def g(k): return W[k].astype(np.float32)
    # --- encoder
    c1 = g("model.encoder.conv1.weight")                            # [d][80][3]
    c2 = g("model.encoder.conv2.weight")                            # [d][d][3]
    w.q8("enc.conv1.w", c1.transpose(0, 2, 1).reshape(d, -1));  w.f32("enc.conv1.b", g("model.encoder.conv1.bias"))
    w.q8("enc.conv2.w", c2.transpose(0, 2, 1).reshape(d, -1));  w.f32("enc.conv2.b", g("model.encoder.conv2.bias"))
    w.f32("enc.pos", g("model.encoder.embed_positions.weight"))
    def attn(prefix, p, with_o=True):
        w.q8(prefix + ".q.w", g(p + ".q_proj.weight"));  w.f32(prefix + ".q.b", g(p + ".q_proj.bias"))
        w.q8(prefix + ".k.w", g(p + ".k_proj.weight"))
        w.q8(prefix + ".v.w", g(p + ".v_proj.weight"));  w.f32(prefix + ".v.b", g(p + ".v_proj.bias"))
        w.q8(prefix + ".o.w", g(p + ".out_proj.weight")); w.f32(prefix + ".o.b", g(p + ".out_proj.bias"))
    def ln(name, p):
        w.f32(name + ".w", g(p + ".weight")); w.f32(name + ".b", g(p + ".bias"))
    for i in range(ne):
        p = "model.encoder.layers.%d" % i
        ln("enc.%d.ln1" % i, p + ".self_attn_layer_norm")
        attn("enc.%d.sa" % i, p + ".self_attn")
        ln("enc.%d.ln2" % i, p + ".final_layer_norm")
        w.q8("enc.%d.fc1.w" % i, g(p + ".fc1.weight")); w.f32("enc.%d.fc1.b" % i, g(p + ".fc1.bias"))
        w.q8("enc.%d.fc2.w" % i, g(p + ".fc2.weight")); w.f32("enc.%d.fc2.b" % i, g(p + ".fc2.bias"))
    ln("enc.ln", "model.encoder.layer_norm")
    # --- decoder
    emb = g("model.decoder.embed_tokens.weight")                    # [vocab][d], also the output matrix
    w.q8("dec.emb", emb)
    w.f32("dec.pos", g("model.decoder.embed_positions.weight"))
    for i in range(nd):
        p = "model.decoder.layers.%d" % i
        ln("dec.%d.ln1" % i, p + ".self_attn_layer_norm")
        attn("dec.%d.sa" % i, p + ".self_attn")
        ln("dec.%d.lnx" % i, p + ".encoder_attn_layer_norm")
        attn("dec.%d.xa" % i, p + ".encoder_attn")
        ln("dec.%d.ln2" % i, p + ".final_layer_norm")
        w.q8("dec.%d.fc1.w" % i, g(p + ".fc1.weight")); w.f32("dec.%d.fc1.b" % i, g(p + ".fc1.bias"))
        w.q8("dec.%d.fc2.w" % i, g(p + ".fc2.weight")); w.f32("dec.%d.fc2.b" % i, g(p + ".fc2.bias"))
    ln("dec.ln", "model.decoder.layer_norm")

    # --- the token table: the UTF-8 bytes of each token (special tokens are empty)
    vj = json.load(open(os.path.join(hf, "vocab.json")))
    added = json.load(open(os.path.join(hf, "added_tokens.json")))
    u2b = {v: k for k, v in bytes_to_unicode().items()}
    toks = [b""] * vocab
    for s, i in vj.items():
        if i < vocab and i < 50257:
            toks[i] = bytes(u2b[ch] for ch in s)
    offs = [0]
    blob = bytearray()
    for t in toks:
        blob += t; offs.append(len(blob))
    w.align()
    toff_pos = w.put(np.array(offs, dtype="<u4").tobytes())
    tblob_pos = w.put(bytes(blob))
    sup = np.array(sorted(set(cfg["suppress_tokens"])), dtype="<i4")
    sup_pos = w.put(sup.tobytes())

    # --- the table of tensors, then the header
    tab = bytearray()
    for (name, kind, rows, cols, cp, doff, soff) in w.table:
        nb = name.encode()
        assert len(nb) < 64
        e = struct.pack("<IIIIQQ", kind, rows, cols, cp, doff, soff) + nb.ljust(64, b"\0")
        assert len(e) == 96, len(e)
        tab += e
    table_off = w.put(bytes(tab))
    hdr = struct.pack("<4sIII" + "I" * 17 + "Q" * 6,
        MAGIC, VERSION, KIND_WHISPER, 0,
        d, nh, ne, nd, ffn, nm, vocab, n_audio, n_text,
        400, 160, 16000,                                   # n_fft, hop, sample rate
        50257, 50256, 50362,                                # sot, eot, notimestamps
        len(w.table), len(sup),
        table_off, toff_pos, tblob_pos, sup_pos, len(blob), 0)
    assert len(hdr) <= 256, len(hdr)
    w.buf[:len(hdr)] = hdr
    w.align()
    with open(out, "wb") as f:
        f.write(bytes(w.buf))
    print("wrote %s: %.1f MB, %d tensors" % (out, len(w.buf) / 1e6, len(w.table)))

if __name__ == "__main__":
    main()
