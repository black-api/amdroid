"""ctypes binding for the Thimble engine (engine/build/libthimble.so).

    from thimble_engine import ThimbleEngine
    eng = ThimbleEngine("model.gguf")
    out = eng.chat([{"role": "user", "content": "Who are you?"}], max_new_tokens=32)
    print(out.reasoning, out.text)

No dependencies beyond the shared library. Set THIMBLE_LIB to override the
library path.
"""

from __future__ import annotations

import ctypes as C
import os
from dataclasses import dataclass, field
from pathlib import Path

_DEFAULT_LIB = Path(__file__).resolve().parents[1] / "build" / "libthimble.so"

THINK_DEFAULT, THINK_OFF, THINK_ON = -1, 0, 1


class _Sampling(C.Structure):
    _fields_ = [
        ("temperature", C.c_float),
        ("top_k", C.c_int),
        ("top_p", C.c_float),
        ("min_p", C.c_float),
        ("repetition_penalty", C.c_float),
        ("seed", C.c_uint64),
        ("frequency_penalty", C.c_float),
        ("presence_penalty", C.c_float),
        ("logit_bias_ids", C.POINTER(C.c_int)),
        ("logit_bias", C.POINTER(C.c_float)),
        ("n_logit_bias", C.c_int),
    ]


class _Message(C.Structure):
    _fields_ = [("role", C.c_char_p), ("content", C.c_char_p)]


class _GenResult(C.Structure):
    _fields_ = [("n_generated", C.c_int), ("stopped_by_eos", C.c_int), ("n_reasoning_tokens", C.c_int)]


_TOKEN_CB = C.CFUNCTYPE(C.c_int, C.c_int, C.POINTER(C.c_char), C.c_int, C.c_int, C.c_void_p)


def _load_lib(path: str | os.PathLike | None = None) -> C.CDLL:
    lib = C.CDLL(str(path or os.environ.get("THIMBLE_LIB") or _DEFAULT_LIB))
    P = C.c_void_p
    sig = {
        "thimble_model_load": (P, [C.c_char_p, C.c_char_p, C.c_size_t]),
        "thimble_model_free": (None, [P]),
        "thimble_n_vocab": (C.c_int, [P]),
        "thimble_n_ctx_train": (C.c_int, [P]),
        "thimble_has_thinking": (C.c_int, [P]),
        "thimble_arch": (C.c_char_p, [P]),
        "thimble_describe": (None, [P, C.c_char_p, C.c_size_t]),
        "thimble_tokenize": (C.c_int, [P, C.c_char_p, C.POINTER(C.c_int), C.c_int]),
        "thimble_token_piece": (C.c_int, [P, C.c_int, C.c_char_p, C.c_int, C.c_int]),
        "thimble_ctx_new": (P, [P, C.c_int]),
        "thimble_ctx_free": (None, [P]),
        "thimble_ctx_reset": (None, [P]),
        "thimble_ctx_pos": (C.c_int, [P]),
        "thimble_ctx_truncate": (C.c_int, [P, C.c_int]),
        "thimble_ctx_copy": (C.c_int, [P, P]),
        "thimble_eval": (C.POINTER(C.c_float), [P, C.c_int]),
        "thimble_render_chat": (C.c_void_p, [P, C.POINTER(_Message), C.c_int, C.c_int]),
        "thimble_generate": (C.c_int, [P, C.POINTER(C.c_int), C.c_int, C.POINTER(_Sampling), C.c_int,
                                       _TOKEN_CB, C.c_void_p, C.POINTER(_GenResult)]),
    }
    for name, (res, args) in sig.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = res, args
    return lib


class Context:
    """An extra inference context sharing the engine's loaded model."""

    def __init__(self, engine: "ThimbleEngine", n_ctx: int = 0):
        self._eng, self._lib = engine, engine._lib
        self._ctx = self._lib.thimble_ctx_new(engine._model, n_ctx)

    @property
    def pos(self) -> int:
        return self._lib.thimble_ctx_pos(self._ctx)

    def eval(self, token: int) -> list[float]:
        ptr = self._lib.thimble_eval(self._ctx, token)
        if not ptr:
            raise RuntimeError("context full or invalid token")
        return ptr[: self._eng.n_vocab]

    def reset(self) -> None:
        self._lib.thimble_ctx_reset(self._ctx)

    def truncate(self, n_keep: int) -> None:
        """Rewind to the first n_keep tokens (works for every layer type)."""
        if self._lib.thimble_ctx_truncate(self._ctx, n_keep):
            raise ValueError("n_keep out of range")

    def copy_from(self, other: "Context") -> None:
        """Fork: take over other's tokens and state."""
        if self._lib.thimble_ctx_copy(self._ctx, other._ctx):
            raise ValueError("cannot copy (different model or too small)")

    def __del__(self):
        if getattr(self, "_ctx", None):
            self._lib.thimble_ctx_free(self._ctx)
            self._ctx = None


@dataclass
class Generation:
    text: str
    reasoning: str = ""
    token_ids: list[int] = field(default_factory=list)
    stopped_by_eos: bool = False


class ThimbleEngine:
    def __init__(self, gguf_path: str | os.PathLike, n_ctx: int = 0, lib: str | None = None):
        self._lib = _load_lib(lib)
        err = C.create_string_buffer(512)
        self._model = self._lib.thimble_model_load(str(gguf_path).encode(), err, len(err))
        if not self._model:
            raise RuntimeError(err.value.decode(errors="replace"))
        self._ctx = self._lib.thimble_ctx_new(self._model, n_ctx)
        self._libc_free = C.CDLL(None).free
        self._libc_free.argtypes = [C.c_void_p]

    # -- info -----------------------------------------------------------------
    @property
    def n_vocab(self) -> int:
        return self._lib.thimble_n_vocab(self._model)

    @property
    def arch(self) -> str:
        return self._lib.thimble_arch(self._model).decode()

    @property
    def thinking(self) -> bool:
        return bool(self._lib.thimble_has_thinking(self._model))

    def describe(self) -> str:
        buf = C.create_string_buffer(4096)
        self._lib.thimble_describe(self._model, buf, len(buf))
        return buf.value.decode()

    # -- tokenizer --------------------------------------------------------------
    def tokenize(self, text: str) -> list[int]:
        raw = text.encode()
        n = self._lib.thimble_tokenize(self._model, raw, None, 0)
        out = (C.c_int * max(n, 1))()
        self._lib.thimble_tokenize(self._model, raw, out, n)
        return list(out[:n])

    def detokenize(self, ids: list[int]) -> str:
        buf = C.create_string_buffer(256)
        data = b""
        for i in ids:
            n = self._lib.thimble_token_piece(self._model, i, buf, len(buf), 0)
            data += buf.raw[:n]
        return data.decode(errors="replace")

    # -- forward ----------------------------------------------------------------
    def new_context(self, n_ctx: int = 0) -> Context:
        return Context(self, n_ctx)

    def reset(self) -> None:
        self._lib.thimble_ctx_reset(self._ctx)

    def eval(self, token: int) -> list[float]:
        ptr = self._lib.thimble_eval(self._ctx, token)
        if not ptr:
            raise RuntimeError("context full or invalid token")
        return ptr[: self.n_vocab]

    def logits(self, ids: list[int]) -> list[list[float]]:
        """Logits for every position of ids (fresh context)."""
        self.reset()
        return [self.eval(i) for i in ids]

    # -- generation ---------------------------------------------------------------
    def render_chat(self, messages: list[dict], thinking: bool | None = None) -> str:
        msgs = (_Message * len(messages))(*[
            _Message(m["role"].encode(), str(m["content"]).encode()) for m in messages
        ])
        mode = THINK_DEFAULT if thinking is None else (THINK_ON if thinking else THINK_OFF)
        ptr = self._lib.thimble_render_chat(self._model, msgs, len(messages), mode)
        if not ptr:
            raise ValueError("thinking requested from a model trained without think mode")
        text = C.string_at(ptr).decode()
        self._libc_free(ptr)
        return text

    def generate(self, prompt: str, max_new_tokens: int = 64, temperature: float = 0.0,
                 top_k: int = 0, top_p: float = 1.0, min_p: float = 0.0,
                 repetition_penalty: float = 1.0, seed: int = 0) -> Generation:
        ids = self.tokenize(prompt)
        arr = (C.c_int * len(ids))(*ids)
        sampling = _Sampling(temperature, top_k, top_p, min_p, repetition_penalty, seed, 0.0, 0.0, None, None, 0)
        reasoning, answer, tokens = bytearray(), bytearray(), []

        def on_token(tok, piece, n, is_reasoning, _user):
            tokens.append(tok)
            (reasoning if is_reasoning else answer).extend(C.string_at(piece, n))
            return 0

        cb = _TOKEN_CB(on_token)
        res = _GenResult()
        self.reset()
        rc = self._lib.thimble_generate(self._ctx, arr, len(ids), C.byref(sampling), max_new_tokens,
                                        cb, None, C.byref(res))
        if rc < 0:
            raise RuntimeError("generation failed (prompt longer than the context?)")
        # the engine never reports <|eos|> to the callback, so tokens excludes it
        return Generation(text=answer.decode(errors="replace").strip(),
                          reasoning=reasoning.decode(errors="replace").strip(),
                          token_ids=tokens, stopped_by_eos=bool(res.stopped_by_eos))

    def chat(self, messages: list[dict], thinking: bool | None = None, **kwargs) -> Generation:
        return self.generate(self.render_chat(messages, thinking), **kwargs)

    def close(self) -> None:
        if getattr(self, "_ctx", None):
            self._lib.thimble_ctx_free(self._ctx)
            self._ctx = None
        if getattr(self, "_model", None):
            self._lib.thimble_model_free(self._model)
            self._model = None

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
