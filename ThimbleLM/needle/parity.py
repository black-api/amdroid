"""C engine (libneedle.so) against the numpy reference: logits and the confidence head."""
import ctypes as C
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import needle_ref as R

HERE = os.path.dirname(os.path.abspath(__file__))
lib = C.CDLL(os.path.join(HERE, "../engine/build/libneedle.so"))
lib.thim_needle_open.restype = C.c_void_p
lib.thim_needle_open.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t]
lib.thim_needle_feed.argtypes = [C.c_void_p, C.POINTER(C.c_int), C.c_int, C.POINTER(C.c_float)]
lib.thim_needle_reset.argtypes = [C.c_void_p]
lib.thim_needle_head.argtypes = [C.c_void_p]
lib.thim_needle_head.restype = C.c_float
lib.thim_needle_encode.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(C.c_int), C.c_int]


def open_c(path):
    data = open(path, "rb").read()
    buf = C.create_string_buffer(data, len(data))
    err = C.create_string_buffer(256)
    t = time.time()
    h = lib.thim_needle_open(buf, len(data), err, 256)
    if not h:
        raise SystemExit("open failed: " + err.value.decode())
    print(f"C open {time.time() - t:.2f}s")
    return h, buf


def c_logits(h, ids, step):
    lib.thim_needle_reset(h)
    out = (C.c_float * 8192)()
    if step:
        for t in ids:
            lib.thim_needle_feed(h, (C.c_int * 1)(t), 1, out)
    else:
        lib.thim_needle_feed(h, (C.c_int * len(ids))(*ids), len(ids), out)
    return np.array(out[:]), lib.thim_needle_head(h)


if __name__ == "__main__":
    cact = sys.argv[1]
    m = R.Model(cact)
    tools = [{"name": "set_volume", "description": "Set the speaker volume.",
              "parameters": {"type": "object", "properties": {"level": {"type": "integer", "minimum": 0, "maximum": 100}},
                             "required": ["level"]}}]
    text = R.render_prefix(tools) + R.render_query("set the volume to 40")
    ids = [R.BOS] + m.tok.encode(text)
    h, keep = open_c(cact)
    cids = (C.c_int * 4096)()
    nc = lib.thim_needle_encode(h, text.encode(), cids, 4096)
    print("tokenizer:", "same" if list(cids[:nc]) == ids[1:] else "DIFFERENT", nc)
    st = m.new_state()
    ref = m.feed(st, ids)[-1]
    ref_head = m.confidence(st)
    for step in (False, True):
        t = time.time()
        got, head = c_logits(h, ids, step)
        dt = time.time() - t
        d = np.abs(got - ref)
        print(f"{'token by token' if step else 'one batch     '}: max|diff| {d.max():.4f} mean {d.mean():.5f} "
              f"(logit scale {np.abs(ref).max():.1f}); argmax C {got.argmax()} ref {ref.argmax()}; "
              f"top5 same {set(np.argsort(-got)[:5]) == set(np.argsort(-ref)[:5])}; head C {head:.4f} ref {ref_head:.4f}; {dt:.2f}s")
