"""Runs Cactus's frozen suites (suites.json) through the numpy reference and scores them the
way cactus/environments/_harness.py does (calls compared casefolded, order-insensitive).
    python run_suites.py needle3.cact suites.json [env ...] [--window N] [--out results.json]
"""
import json
import math
import sys
import time

import numpy as np
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import needle_ref as R


def fold(v):
    if isinstance(v, str):
        return v.casefold()
    if isinstance(v, float) and v.is_integer():
        return int(v)
    if isinstance(v, dict):
        return {k: fold(x) for k, x in v.items()}
    if isinstance(v, list):
        return [fold(x) for x in v]
    return v


def key(call):
    return json.dumps(fold(call), sort_keys=True)


def parse_calls(text):
    a = text.find("<tool_call>")
    b = text.find("</tool_call>")
    if a < 0 or b < 0:
        return None
    try:
        calls = json.loads(text[a + len("<tool_call>"):b])
    except json.JSONDecodeError:
        return None
    return calls if isinstance(calls, list) else None


def main():
    args = sys.argv[1:]
    window, out_path = None, None
    if "--window" in args:
        i = args.index("--window"); window = int(args[i + 1]); del args[i:i + 2]
    if "--out" in args:
        i = args.index("--out"); out_path = args[i + 1]; del args[i:i + 2]
    cact, suites = args[0], json.load(open(args[1]))
    envs = args[2:] or list(suites)
    m = R.Model(cact, local_window=window)
    results, total, passed = [], 0, 0
    for env in envs:
        s = suites[env]
        prefix = [R.BOS] + m.tok.encode(R.render_prefix(s["tools"], s["system"]))
        base = m.new_state()
        m.feed(base, prefix)
        ep = 0
        for case in s["cases"]:
            t0 = time.time()
            st = m.copy_state(base)
            q = m.tok.encode(R.render_query(case["query"]))
            logits = m.feed(st, q)[-1]
            out, logp_call, in_call = [], 0.0, False
            for _ in range(160):
                p = np.exp(logits - logits.max()); p /= p.sum()
                t = int(np.argmax(logits))
                if t == R.EOS:
                    break
                piece = m.tok.pieces[t]
                if piece == "<tool_call>":
                    in_call = True
                if in_call:
                    logp_call += math.log(max(p[t], 1e-30))
                out.append(t)
                logits = m.feed(st, [t])[-1]
                if piece == "<|im_end|>":
                    break
            text = "".join(m.tok.pieces[t] for t in out).replace(R.SP, " ").replace("<0x0A>", "\n")
            conf = m.confidence(st)
            calls = parse_calls(text)
            got = calls or []
            ok = sorted(key(c) for c in got) == sorted(key(c) for c in case["calls"])
            total += 1; passed += ok; ep += ok
            r = dict(env=env, query=case["query"], want=case["calls"], got=calls, ok=ok, category=case["category"],
                     critical=case.get("critical", False), head=conf, decode=math.exp(logp_call), text=text,
                     prompt_tokens=len(prefix) + len(q), out_tokens=len(out), seconds=round(time.time() - t0, 2))
            results.append(r)
            print(("PASS " if ok else "FAIL ") + f"[{env}/{case['category']}] {case['query']!r} -> {json.dumps(calls)} "
                  f"head={conf:.2f} dec={r['decode']:.2f} {r['seconds']}s", flush=True)
        print(f"== {env}: {ep}/{len(s['cases'])}", flush=True)
    print(f"TOTAL {passed}/{total}")
    if out_path:
        json.dump(results, open(out_path, "w"), indent=1)


if __name__ == "__main__":
    main()
