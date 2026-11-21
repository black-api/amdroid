"""The frozen suites through the C engine (grammar-constrained decoding + confidence).
    python run_suites_c.py needle3.cact suites.json [env ...] [--window N] [--gate G] [--out f.json]
"""
import ctypes as C
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_suites import key

HERE = os.path.dirname(os.path.abspath(__file__))
lib = C.CDLL(os.path.join(HERE, "../engine/build/" + os.environ.get("NEEDLE_LIB", "libneedle_g.so")))


class Res(C.Structure):
    _fields_ = [("confidence", C.c_float), ("head", C.c_float), ("decode", C.c_float), ("n_calls", C.c_int),
                ("withheld", C.c_int), ("prompt_tokens", C.c_int), ("new_tokens", C.c_int), ("prefill_us", C.c_double), ("decode_us", C.c_double)]


class Opts(C.Structure):
    _fields_ = [("max_new_tokens", C.c_int), ("max_think_tokens", C.c_int), ("no_refusal", C.c_int), ("local_window", C.c_int),
                ("gates", C.c_int), ("floor", C.c_float)]


lib.thim_needle_open.restype = C.c_void_p
lib.thim_needle_open.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.c_size_t]
lib.thim_needle_set_tools.argtypes = [C.c_void_p, C.c_char_p, C.c_char_p, C.c_char_p, C.c_size_t]
lib.thim_needle_run.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(Opts), C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t, C.POINTER(Res)]
CLOCK = C.CFUNCTYPE(C.c_double)(lambda: time.perf_counter() * 1e6)
lib.thim_needle_set_clock(CLOCK)


def main():
    args = sys.argv[1:]
    opts = Opts(0, 0, 0, 0, 0, 0.0)
    gate, out_path = 0.0, None
    if "--gates" in args:
        i = args.index("--gates"); opts.gates = int(args[i + 1]); del args[i:i + 2]
    if "--think" in args:
        i = args.index("--think"); opts.max_think_tokens = int(args[i + 1]); del args[i:i + 2]
    if "--window" in args:
        i = args.index("--window"); opts.local_window = int(args[i + 1]); del args[i:i + 2]
    if "--gate" in args:
        i = args.index("--gate"); gate = float(args[i + 1]); del args[i:i + 2]
    if "--out" in args:
        i = args.index("--out"); out_path = args[i + 1]; del args[i:i + 2]
    data = open(args[0], "rb").read()
    buf = C.create_string_buffer(data, len(data))
    err = C.create_string_buffer(256)
    h = lib.thim_needle_open(buf, len(data), err, 256)
    if not h:
        raise SystemExit(err.value.decode())
    suites = json.load(open(args[1]))
    envs = args[2:] or list(suites)
    total = passed = 0
    results = []
    for env in envs:
        s = suites[env]
        t = time.time()
        tools = json.dumps(s["tools"], separators=(",", ":"), ensure_ascii=False)
        if lib.thim_needle_set_tools(h, tools.encode(), s["system"].encode(), err, 256) != 0:
            raise SystemExit(err.value.decode())
        print(f"== {env}: tools prefix in {time.time() - t:.2f}s", flush=True)
        ep = 0
        for case in s["cases"]:
            calls = C.create_string_buffer(4096)
            reason = C.create_string_buffer(1024)
            r = Res()
            rc = lib.thim_needle_run(h, case["query"].encode(), C.byref(opts), calls, 4096, reason, 1024, C.byref(r))
            try:
                got = json.loads(calls.value.decode()) if rc == 0 else []
            except json.JSONDecodeError:
                got = None
            acted = got if (got and r.withheld == 0 and r.confidence >= max(gate, 0.1)) else []
            ok = sorted(key(c) for c in acted) == sorted(key(c) for c in case["calls"])
            total += 1; passed += ok; ep += ok
            results.append(dict(env=env, query=case["query"], want=case["calls"], got=got, ok=ok, category=case["category"],
                                confidence=r.confidence, head=r.head, decode=r.decode, withheld=r.withheld, reasoning=reason.value.decode()))
            print(("PASS " if ok else "FAIL ") + f"[{case['category']}] {case['query']!r} -> {calls.value.decode()} "
                  f"conf={r.confidence:.2f} (head {r.head:.2f} dec {r.decode:.2f}){' WITHHELD ' + str(r.withheld) if r.withheld else ''} "
                  f"{r.prompt_tokens}+{r.new_tokens} tok {r.prefill_us / 1e3:.0f}+{r.decode_us / 1e3:.0f} ms", flush=True)
        print(f"== {env}: {ep}/{len(s['cases'])}", flush=True)
    print(f"TOTAL {passed}/{total} (gate {max(gate, 0.1)})")
    if out_path:
        json.dump(results, open(out_path, "w"), indent=1)


if __name__ == "__main__":
    main()
