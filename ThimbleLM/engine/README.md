# Thimble engine

A small, dependency-free C11 inference engine for ThimbleLM models: **thimble-v1,
thimble-v2 and thimble-v4**, including GQA, MoE, the v4 hybrid
short-conv/attention stack and think mode. It runs the GGUF files written by
`python3 -m thimble export-gguf` (GGUF archs `thimble` and `thimble-hybrid`).
It needs no PyTorch and no llama.cpp patch.

```bash
make -C engine                       # build/thimble, build/libthimble.{a,so}
make -C engine OPENMP=1              # parallel matmuls (bigger models)

python3 -m thimble export-gguf runs/thimble-1m-cpu/sft/final model.gguf

engine/build/thimble info  model.gguf
engine/build/thimble run   model.gguf --chat "Who are you?"
engine/build/thimble run   model.gguf "the capital of france is" -n 32 --temp 0.8 --top-p 0.9
engine/build/thimble chat  model.gguf --show-think        # interactive; think models stream their trace
engine/build/thimble tokenize model.gguf "<|user|>hi<|assistant|>"
engine/build/thimble logits   model.gguf 5 106 74          # one line of logits per position
engine/build/thimble serve    model.gguf --port 8080       # OpenAI-compatible HTTP API
```

Options for `run` and `chat`:
- `-n N`
- `--temp T`, `--top-k K`, `--top-p P`, `--min-p P`
- `--repeat-penalty R`
- `--seed S`
- `--chat`
- `--think` / `--no-think` (default: on for models trained with `thinking: true`)
- `--show-think`
- `--ctx N`

## Voice (text to speech)

The engine speaks with a ThimbleTTS voice packed into one `.voice` file - the
three networks, both pronunciation dictionaries and the out-of-vocabulary
fallback model in a single safetensors bundle. No Python, no PyTorch, no data
files next to the binary.

```bash
python3 -m thimble voice install models/thimbletts-1m-preview   # -> ~/.cache/thimble/voices
thimble speak "Two plus two equals four." --out answer.wav
thimble run model.gguf --chat "Who are you?" | thimble speak -   # the model reads its answer
thimble voice                                   # which voice is installed
thimble serve model.gguf                        # POST /v1/audio/speech (wav | pcm)
```

`speak` takes the text as an argument, or `-` (or piped stdin) to read it all
from standard input; `--out -` writes the WAV to stdout.

Lookup order: `--voice NAME|PATH` (`off` disables speech) > `$THIMBLE_VOICE` >
`$THIMBLE_VOICE_DIR` > `$XDG_CACHE_HOME/thimble/voices` > `~/.cache/thimble/voices`.

C API: `thimble_voice_load`, `thimble_voice_speak` (16-bit mono PCM),
`thimble_voice_find`/`_dir`, plus `thimble_voice_ids`, `_phonemes` and
`_synth_ids` for tests. Sources: `tts_g2p.c` (English front end, a port of
`thimble/tts/g2p/english.py`), `tts_oov.c` (the fallback seq2seq model),
`tts_net.c` (duration, acoustic, ConvNeXt decoder, iSTFT, safetensors reader),
`tts_voice.c` (loading, chunking, normalisation). `tools/gen_tts_tables.py`
regenerates `src/tts_tables.h` from the Python front end.

Synthesis is multi-threaded (`--threads N`, `$THIMBLE_VOICE_THREADS`, server
flag `--voice-threads`; default = cores, capped at 8). The threads only split
work that is already independent, so the waveform is bit-identical whatever the
thread count.

On this machine (4 cores, i5-2500): 4 ms to load, 2.1 s of audio in 13 ms and
13.4 s in 82 ms - about **165x real time** on 4 threads, 65x on one - with about
22 MB peak memory. Waveforms are within 86-118 dB SNR of the PyTorch runtime and
phoneme strings are identical on 3,735 test sentences.

## Copy head (pointer output layer)

A model exported with `copy_head: true` runs here unchanged — `thimble info`
reports `copy head: yes`. After the usual logits, the engine mixes in an
attention over the tokens seen so far, scattered onto their vocabulary ids:

```
p(v) = g · softmax(logits)[v] + (1 − g) · Σ_j attn[j] · [ctx[j] == v]
```

`thimble_eval` then returns **log p** rather than raw logits, exactly as the
PyTorch side does, so temperature, top-k, top-p, min-p and the repeat penalty
keep behaving the same way.

Two things this needs that nothing else in the engine did:

- **The context token ids are kept** (`ctx->ctx_tokens`), because they are what
  the pointer copies from. They are carried by `thimble_ctx_copy` and preserved
  by `thimble_ctx_truncate`.
- **Pointer keys are cached per position** (`ctx->ckeys`). A key depends only on
  its token, so it is projected once when the position is first seen.
  Recomputing the whole context each step cost 17% of decode throughput; cached,
  the head costs **6.8%** (2,070 → 1,928 tok/s on the showcase model) and
  +0.1 MB.

Parity with PyTorch is covered by `tests/test_copy_head.py`: identical argmax
and `max |Δp| < 2e-5` with the gate opened so half of each distribution comes
through the pointer, for both learned positions and RoPE.

## Quantization

Pick a weight format at export time. The blocks use ggml's bit layout, so the
same file also loads in the patched llama.cpp.

```bash
python3 -m thimble export-gguf CHECKPOINT model-q4_0.gguf --outtype q4_0
```

The formats are `f32` (the default, and exact), `f16`, `bf16`, `q8_0`,
`q5_1`, `q5_0`, `q4_1` and `q4_0`.
- **Weight matrices** use the chosen format.
- **`token_embd` and `output`** stay at Q8_0 for the 4- and 5-bit types.
- **Norms, conv taps,** and matrices whose rows aren't a multiple of 32 stay F32.
- **The copy head** (`copy_q`, `copy_k`, `copy_gate`) stays F32 at every
  format. The gate scales the whole pointer distribution, so rounding it moves
  every token's probability, and the tensors are only a few KB.

Measured on `thimble-1m-cpu` with `python3 examples/quant_report.py`, using
held-out (unseen-phrasing) perplexity, the 8 probes, and greedy speed on 4
loaded threads:

| Format | Size | vs F32 | Val ppl | Probes | tok/s |
|---|---:|---:|---:|---:|---:|
| f32 | 4.35 MB | 100% | 2.121 | 5/8 | ~1,090 |
| f16 | 2.19 MB | 50% | 2.121 | 5/8 | ~740 |
| bf16 | 2.19 MB | 50% | 2.121 | 5/8 | ~1,100 |
| q8_0 | 1.18 MB | 27% | 2.121 | 5/8 | ~1,250 |
| q5_1 | 0.90 MB | 21% | 2.121 | 5/8 | ~610 |
| q5_0 | 0.84 MB | 19% | 2.120 | 5/8 | ~590 |
| q4_1 | 0.79 MB | 18% | 2.119 | 5/8 | ~1,480 |
| **q4_0** | **0.73 MB** | **17%** | **2.119** | **5/8** | **~1,710** |

At this scale quantization costs no measurable quality, and Q4_0 is the
smallest and fastest format. The Q5 formats unpack their 5th bit per element,
which makes them slower than Q4.

**Exactness.** A quantized GGUF computes exactly what its dequantized F32 twin
computes, within 1e-4 relative. This is tested for every architecture case and
every format, 84 combinations (`test_engine_quantized_formats_exact`). The
patched llama.cpp loads all 8 formats with 100% argmax agreement with the
engine. Its 4/5-bit kernels also quantize activations, so their logits differ
from the engine's by up to about 0.03.

K-quants (Q4_K to Q6_K) are not offered. gguf-py can't produce them
bit-exactly, and they need rows of 256, which most ThimbleLM matrices don't
have. To add a format, see the comment at the top of `src/quant.h`.

## HTTP server

```bash
engine/build/thimble serve model.gguf                                   # http://127.0.0.1:8080
engine/build/thimble serve a.gguf --model b.gguf=fast --parallel 4 --api-key KEY --system-prompt "Be brief."
engine/build/thimble serve --config server.json                         # everything from a file
engine/build/thimble serve --help                                       # all options
```

### What it does

**Serving several requests at once**
- A pool of worker threads (`--parallel`, default 2) serves requests
  concurrently. Each worker has its own context per model; the weights are
  shared.
- HTTP keep-alive is on by default (`--keepalive` seconds).
- When the request queue is full (`--queue`), the server answers 503 instead
  of hanging.

**Several models**
- Pass several GGUFs, or `--model PATH=ALIAS`, and clients choose one with
  `"model"`.
- With a single model, any model name is accepted, so stock OpenAI clients
  work unchanged.

**Prompt cache.** Each worker remembers the tokens in its context.
- A request that extends the previous prompt, such as the next chat turn,
  only evaluates the new tokens.
- A prompt that shares just a prefix rewinds the cache to that prefix. This
  works for every architecture, including v4 hybrids, because short-conv
  layers keep their input history per position.
- The response reports the reuse in `usage.prompt_tokens_details.cached_tokens`.
- Results are identical with and without the cache (tested).
- Turn it off per request with `"cache_prompt": false`, or globally with
  `--no-prompt-cache`.

**Context management**
- `max_tokens` is clamped to the context that remains.
- Chats that don't fit drop their oldest turns, keeping system messages and
  the latest message (`--trim-history`, `--min-gen`). The count is reported as
  `timings.dropped_messages`.
- For completions, `"truncate": true` keeps the end of a prompt that is too
  long.

**Streaming**
- Text that might be the start of a stop string is held back until it's
  resolved, so a stop string that spans tokens never leaks into the stream.
- Split UTF-8 characters are never emitted.
- `stream_options.include_usage` adds a final usage chunk.

**OpenAI-compatible request fields**
- Input: `messages` (roles system / `developer` / user / assistant; text
  content parts accepted) and `prompt`.
- Output size and count: `max_tokens` / `max_completion_tokens`, `n` (several
  choices: the prompt is evaluated once, forked with `thimble_ctx_copy`, and
  the choices are generated in parallel threads), `stream`.
- Sampling: `temperature`, `top_p`, `seed`, `frequency_penalty`,
  `presence_penalty`, `logit_bias`, `stop`.
- Log probabilities: `logprobs` + `top_logprobs` (up to 20), in the
  `{"content": [...]}` format.
- Think mode: `enable_thinking`, `chat_template_kwargs.enable_thinking` or
  `reasoning_effort: "none"`.
- Extensions: `top_k`, `min_p`, `repetition_penalty` / `repeat_penalty`,
  `ignore_eos`, `cache_prompt`, `truncate`, `include_reasoning`.

**Responses**
- Think models return `reasoning_content`.
- Every response carries `usage` plus `timings`: prompt and generation
  milliseconds, tokens per second, and dropped messages.

| Endpoint | |
|---|---|
| `POST /v1/chat/completions`, `POST /v1/completions` | Also without the `/v1` prefix |
| `POST /tokenize`, `POST /detokenize` | `{"content": ..., "with_pieces": true}` and `{"tokens": [...]}` |
| `GET /v1/models` | Alias, arch, think mode, context length and vocabulary for each model |
| `GET /props` | Effective settings (API key redacted) and a description of each model |
| `GET /metrics` | Prometheus counters: requests, errors, tokens, cached tokens, dropped turns, timings, active requests |
| `GET /health` | Always open, even with an API key |
| `GET /` | Chat page: streaming, model picker, think toggle, temperature and max tokens, live tok/s |

### Settings

Settings come from the defaults, then a JSON `--config` file, then the
command-line flags, which win. Config keys are the flag names with `-`
replaced by `_`, plus a `models` list:

```json
{
  "host": "0.0.0.0", "port": 8080, "parallel": 4, "api_key": "change-me",
  "models": ["models/thimble-1m-q4_0.gguf", {"path": "models/v4-think.gguf", "alias": "thinker"}],
  "system_prompt": "You are Thimble, a small assistant.",
  "temp": 0.7, "top_p": 0.9, "max_tokens": 128, "max_tokens_limit": 512,
  "think": "default", "show_reasoning": true, "trim_history": true, "log": "requests"
}
```

| Group | Flags |
|---|---|
| Network | `--host` `--port` `--parallel` `--queue` `--keepalive` `--timeout` `--max-body` `--cors` (empty = off) `--api-key` (or env `THIMBLE_API_KEY`) |
| Features | `--webui` `--metrics` `--prompt-cache` `--trim-history` `--show-reasoning`; booleans take `on`/`off` and `--no-NAME` |
| Models | positional paths, `--model PATH[=ALIAS]`, `--alias`, `--ctx` |
| Defaults for requests | `--temp` `--top-k` `--top-p` `--min-p` `--repeat-penalty` `--frequency-penalty` `--presence-penalty` `--max-tokens` |
| Limits | `--max-tokens-limit` (hard cap) `--max-n` `--min-gen` |
| Behaviour | `--think default\|on\|off` `--system-prompt` / `--system-prompt-file` (used when a chat has no system message) `--log quiet\|requests\|verbose` |

### Tests

`tests/test_engine.py::test_server_*` starts real servers and checks that:
- chat output matches the Python engine with thinking on and off,
- streaming reassembles to the non-streamed answer, including stop strings
  that span tokens,
- the config file, auth, routing by `model`, `/props`, `/metrics` and
  tokenize/detokenize behave correctly,
- the default system prompt, the `developer` role and content parts are handled,
- the prompt cache (extend and rewind, including v4 hybrids) reuses tokens
  without changing outputs,
- parallel `n` choices: greedy choices all equal the single answer, choice 0
  equals the `n=1` sample, and streamed choices match the non-streamed ones,
- 18 concurrent requests on 3 workers return exactly the sequential results,
- logprobs match the model's log-softmax,
- `logit_bias`, `ignore_eos`, `n`, the `max_n` cap and context clamping work,
- history trimming drops turns as expected.

The server is plain C with pthreads and no other dependencies. Its code lives
in `tools/` and is split by concern:

| File | Concern |
|---|---|
| `http.c` | Sockets, workers, keep-alive, SSE |
| `json.c` | JSON parsing and writing |
| `server_config.c` | One option table for flags, config file and `--help` |
| `server_gen.c` | Prompt building, cache, sampling loop, responses |
| `server.c` | Routing, auth, endpoints, metrics, web UI |

## Exactness and speed

`tests/test_engine.py` builds the engine and checks it against PyTorch.

**Logits.** Fourteen positions per configuration match PyTorch within 1e-4
relative, with the same argmax, for every architecture case in the GGUF test
matrix:
- v1/v2/v4
- GELU / SwiGLU
- RMSNorm / LayerNorm
- RoPE / learned positions
- tied / untied embeddings
- GQA, MoE, hybrid + MoE
- v4 with and without the attention gate
- think mode

**Tokenizer.** Output is identical to HF `tokenizers` on strings with special
tokens, contractions, whitespace runs, digits, punctuation, characters missing
from the vocabulary, and non-ASCII text.

**Generation.** A trained think-mode v4 model gives the same reasoning, answer
and token ids as `thimble.infer.Engine` with thinking on, off and default. The
CLI streams the same answer.

On the published `thimble-1m-cpu` model, on 4 CPU threads under load:

| | Thimble engine | PyTorch engine |
|---|---|---|
| Load time | 1 ms | 208 ms |
| Greedy generation | ~1,100 tok/s | ~50 tok/s |
| Size | 57 KB `.so`, 66 KB CLI | needs PyTorch |

Both produce identical tokens.

## Python

`python/thimble_engine.py` is a `ctypes` binding with no dependencies:

```python
import sys; sys.path.insert(0, "engine/python")
from thimble_engine import ThimbleEngine

eng = ThimbleEngine("model.gguf")
out = eng.chat([{"role": "user", "content": "What is 3 plus 4?"}], max_new_tokens=32)
print(out.reasoning, "|", out.text)
eng.logits(eng.tokenize("hello"))     # per-position logits
```

## C API

The C API is declared in `include/thimble.h`. In outline:

```c
thimble_model *m = thimble_model_load("model.gguf", err, sizeof err);
thimble_ctx   *c = thimble_ctx_new(m, 0);                        /* 0 = training context */
thimble_message msg = { "user", "Who are you?" };
char *prompt = thimble_render_chat(m, &msg, 1, THIMBLE_THINK_DEFAULT);
int n = thimble_tokenize(m, prompt, NULL, 0); /* ... allocate ids, tokenize again ... */
thimble_sampling s = thimble_sampling_default();                   /* greedy */
thimble_generate(c, ids, n, &s, 64, on_token, user, &result);      /* streams pieces */
```

Single steps are available too: `thimble_eval(ctx, token)` appends one token
and returns its logits.

Two more calls work for every architecture:
- `thimble_ctx_truncate(ctx, n)` rewinds a context to its first `n` tokens.
- `thimble_ctx_copy(dst, src)` forks a context, copying only the positions in
  use.

Rewinding is possible for v4 hybrids too, because a short-conv layer stores
its input `B*u` per position, as attention stores K/V. Its window is read
from that history. Both calls give exactly the logits of a fresh run (tested).
In Python they are `eng.new_context()` and `Context.truncate()` /
`Context.copy_from()`.

## Layout (one concern per file)

| File | What it does |
|---|---|
| `include/thimble.h` | Public API |
| `src/gguf.c/.h` | Read-only GGUF v2/v3 parser (mmap) |
| `src/model.c/.h` | Hyperparameters, global tensors, **layer registry**, `model_tensor()` shape-checked lookup |
| `src/forward.c` | Context allocation and the per-token forward pass |
| `src/mixer_attn.c` | Attention: GQA, QK-norm, RoPE, KV cache, v4 sigmoid output gate |
| `src/mixer_conv.c` | v4 gated short convolution; its input history is kept per position (rewindable) |
| `src/ffn_dense.c` | GELU(erf) or SwiGLU FFN |
| `src/ffn_moe.c` | Softmax top-k MoE with renormalised gates (SwiGLU experts) |
| `src/ops.c/.h` | Kernels: matvec (F32 and any format via `op_matvec_w`), RMSNorm, LayerNorm, RoPE, softmax, activations |
| `src/quant.c/.h` | Weight formats: F16/BF16/Q8_0/Q5_1/Q5_0/Q4_1/Q4_0 dequantization and fused dot kernels |
| `src/tokenizer.c/.h` | Byte-level BPE (GPT-2 pre-tokenizer, special tokens, unk) |
| `src/sampler.c/.h` | Greedy, temperature, top-k, top-p, min-p, repetition penalty (seeded) |
| `src/chat.c` | ThimbleLM chat template and think-mode prompts |
| `src/generate.c` | Generation loop that splits reasoning from the answer |
| `src/util.c/.h` | Hash maps and UTF-8 helpers |
| `tools/thimble_cli.c` | The `thimble` CLI |
| `tools/server.c/.h` | Server entry, routing, auth, `/props` `/metrics` `/tokenize`, web UI |
| `tools/server_gen.c` | Chat/completions: prompt building, history trimming, prompt cache, sampling loop, logprobs, streaming |
| `tools/server_config.c/.h` | Settings table shared by the CLI flags, the JSON config and `--help` |
| `tools/server_internal.h` | Shared server state: models, per-worker slots, metrics |
| `tools/http.c/.h` | HTTP/1.1: worker pool, queue, keep-alive, SSE |
| `tools/json.c/.h` | Small JSON parser and escaping string builder |
| `python/thimble_engine.py` | `ctypes` binding |

### Adding a new layer type

Every block is `x += mixer(norm1(x)); x += ffn(norm2(x))`. Mixers and FFNs are
plugged in through small descriptors, defined in `src/model.h`:

```c
typedef struct thimble_mixer {
    const char *name;
    int    (*load)(thimble_model *m, thimble_layer *L, int il);                 /* bind weights */
    size_t (*state_floats)(const thimble_model *m, const thimble_layer *L, int n_ctx);
    void   (*forward)(thimble_ctx *c, const thimble_layer *L, float *state,
                      const float *in, float *out);
} thimble_mixer;
```

To add one:
1. Create `src/mixer_<name>.c` (or `src/ffn_<name>.c`). It binds its matmul
   weights with `model_weight(m, il, "my_tensor.weight", required, ne0, ne1, ne2)`,
   which works in any format and is applied with `op_matvec_w`. F32 vectors
   such as norms use `model_tensor(...)`. It also implements `forward`. Its per-sequence state, such as a cache, comes from
   `state_floats` and is zeroed by `thimble_ctx_reset`.
2. Declare it in `src/model.h` (`extern const thimble_mixer MIXER_<NAME>;`).
3. Select it in `choose_mixer()` / `choose_ffn()` in `src/model.c`, usually by
   checking which tensors or keys the GGUF contains.
4. Write the matching tensors in `thimble/export/gguf.py`, and add a case to
   `GGUF_CASES` in `tests/test_thimble.py`. The engine and llama.cpp
   exactness tests then pick it up automatically.

The Makefile compiles every file in `src/`, so a new file needs no build change.

## Limitations

- **No K-quants or I-quants.** Supported formats are F32, F16, BF16, Q8_0,
  Q5_1, Q5_0, Q4_1 and Q4_0.
- **Single sequence, one token per step.** Prefill also runs token by token,
  which is fine at ThimbleLM sizes.
- **Unicode in the pre-tokenizer is approximated.** ASCII is exact.
  Non-ASCII characters count as letters, except whitespace and punctuation
  blocks. All texts in the test suite match HF exactly.
- **Not supported:** `attn_window > 0`, and MoE with GELU experts. The
  exporter rejects both.
- **A copy-head model needs the `thimble` arch.** Stock llama.cpp has no
  pointer output layer, so `--target stock` is rejected rather than producing a
  file that loads and answers differently.
