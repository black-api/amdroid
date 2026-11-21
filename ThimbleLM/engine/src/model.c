/* Model loading: hyperparameters, global tensors, and per-layer dispatch. */
#include "model.h"
#include "ops.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int model_error(thimble_model *m, const char *fmt, ...) {
    if (m->err && m->err_len && !m->err[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(m->err, m->err_len, fmt, ap);
        va_end(ap);
    }
    return -1;
}

const float *model_tensor(thimble_model *m, int il, const char *name, int required,
                          int64_t ne0, int64_t ne1, int64_t ne2) {
    char full[128];
    if (il >= 0) snprintf(full, sizeof(full), "blk.%d.%s", il, name);
    else         snprintf(full, sizeof(full), "%s", name);
    const gguf_tensor *t = gguf_find_tensor(&m->gguf, full);
    if (!t) {
        if (required) model_error(m, "missing tensor %s", full);
        return NULL;
    }
    if (t->type != GGML_TYPE_F32) {
        model_error(m, "tensor %s: only F32 is supported (re-export with thimble export-gguf)", full);
        return NULL;
    }
    int64_t want[3] = { ne0, ne1, ne2 };
    for (int d = 0; d < 3; d++) {
        if (want[d] >= 0 && t->ne[d] != want[d]) {
            model_error(m, "tensor %s: dim %d is %lld, expected %lld", full, d,
                        (long long) t->ne[d], (long long) want[d]);
            return NULL;
        }
    }
    return (const float *) t->data;
}

thimble_weight model_weight(thimble_model *m, int il, const char *name, int required,
                            int64_t ne0, int64_t ne1, int64_t ne2) {
    thimble_weight w = { 0 };
    char full[128];
    if (il >= 0) snprintf(full, sizeof(full), "blk.%d.%s", il, name);
    else         snprintf(full, sizeof(full), "%s", name);
    const gguf_tensor *t = gguf_find_tensor(&m->gguf, full);
    if (!t) {
        if (required) model_error(m, "missing tensor %s", full);
        return w;
    }
    if (!quant_supported((int) t->type)) {
        model_error(m, "tensor %s: unsupported type %u (supported: F32 F16 BF16 Q8_0 Q5_1 Q5_0 Q4_1 Q4_0)",
                    full, t->type);
        return w;
    }
    int64_t want[3] = { ne0, ne1, ne2 };
    for (int d = 0; d < 3; d++) {
        if (want[d] >= 0 && t->ne[d] != want[d]) {
            model_error(m, "tensor %s: dim %d is %lld, expected %lld", full, d,
                        (long long) t->ne[d], (long long) want[d]);
            return w;
        }
    }
    w.row_bytes = quant_row_bytes((int) t->type, t->ne[0]);
    if (!w.row_bytes) {
        model_error(m, "tensor %s: row length %lld is not a multiple of the %s block size", full,
                    (long long) t->ne[0], quant_name((int) t->type));
        return w;
    }
    w.data = t->data;
    w.type = (int) t->type;
    for (int d = 0; d < 3; d++) w.ne[d] = t->ne[d];
    return w;
}

void model_norm(const thimble_model *m, float *out, const float *x, const float *w, const float *b) {
    if (m->hp.layer_norm) op_layernorm(out, x, w, b, m->hp.n_embd, m->hp.ln_eps);
    else                  op_rmsnorm(out, x, w, m->hp.n_embd, m->hp.rms_eps);
}

/* ---- hyperparameters -------------------------------------------------- */

static int64_t kv_int(thimble_model *m, const char *suffix, int64_t def) {
    char key[128];
    int64_t v;
    snprintf(key, sizeof(key), "%s.%s", m->arch, suffix);
    return gguf_get_int(&m->gguf, key, &v) ? v : def;
}

static double kv_float(thimble_model *m, const char *suffix, double def) {
    char key[128];
    double v;
    snprintf(key, sizeof(key), "%s.%s", m->arch, suffix);
    return gguf_get_float(&m->gguf, key, &v) ? v : def;
}

/* head_count_kv may be a scalar or a per-layer array (0 = short-conv layer). */
static int kv_heads(thimble_model *m, int il) {
    char key[128];
    snprintf(key, sizeof(key), "%s.attention.head_count_kv", m->arch);
    const gguf_kv *kv = gguf_find_kv(&m->gguf, key);
    if (!kv) return m->hp.n_head;
    if (kv->type == GGUF_ARRAY) return (int) gguf_arr_int(kv, (uint64_t) il);
    int64_t v;
    return gguf_get_int(&m->gguf, key, &v) ? (int) v : m->hp.n_head;
}

static int load_hparams(thimble_model *m) {
    thimble_hparams *hp = &m->hp;
    hp->n_embd   = (int) kv_int(m, "embedding_length", 0);
    hp->n_layer  = (int) kv_int(m, "block_count", 0);
    hp->n_head   = (int) kv_int(m, "attention.head_count", 0);
    hp->n_ff     = (int) kv_int(m, "feed_forward_length", 0);
    if (hp->n_embd <= 0 || hp->n_layer <= 0 || hp->n_head <= 0)
        return model_error(m, "missing or invalid %s.* hyperparameters", m->arch);
    hp->head_dim       = (int) kv_int(m, "attention.key_length", hp->n_embd / hp->n_head);
    hp->n_ctx_train    = (int) kv_int(m, "context_length", 2048);
    hp->n_expert       = (int) kv_int(m, "expert_count", 0);
    hp->n_expert_used  = (int) kv_int(m, "expert_used_count", 0);
    hp->n_ff_exp       = (int) kv_int(m, "expert_feed_forward_length", hp->n_ff);
    hp->conv_kernel    = (int) kv_int(m, "shortconv.l_cache", 3);
    hp->rope_base      = (float) kv_float(m, "rope.freq_base", 10000.0);
    hp->rms_eps        = (float) kv_float(m, "attention.layer_norm_rms_epsilon", 1e-6);
    hp->ln_eps         = (float) kv_float(m, "attention.layer_norm_epsilon", 1e-5);
    char key[128];
    snprintf(key, sizeof(key), "%s.thinking", m->arch);
    if (!gguf_get_bool(&m->gguf, key, &hp->thinking)) hp->thinking = 0;
    snprintf(key, sizeof(key), "%s.copy_head", m->arch);
    if (!gguf_get_bool(&m->gguf, key, &hp->copy_head)) hp->copy_head = 0;
    hp->copy_dim = hp->copy_head ? (int) kv_int(m, "copy_dim", hp->head_dim) : 0;
    if (hp->copy_head && hp->copy_dim <= 0)
        return model_error(m, "invalid %s.copy_dim %d", m->arch, hp->copy_dim);
    if (hp->n_expert > 0 && (hp->n_expert_used <= 0 || hp->n_expert_used > hp->n_expert))
        return model_error(m, "invalid expert_used_count %d", hp->n_expert_used);
    return 0;
}

/* ---- layer dispatch: pick implementations from the tensors present ---- */

static const thimble_mixer *choose_mixer(thimble_model *m, int il) {
    if (strcmp(m->arch, "thimble-hybrid") == 0 && kv_heads(m, il) == 0) return &MIXER_SHORTCONV;
    return &MIXER_ATTENTION;
}

static const thimble_ffn *choose_ffn(thimble_model *m, int il) {
    (void) il;
    return m->hp.n_expert > 0 ? &FFN_MOE : &FFN_DENSE;
}

static int load_layers(thimble_model *m) {
    const int d = m->hp.n_embd;
    m->layers = calloc((size_t) m->hp.n_layer, sizeof(thimble_layer));
    if (!m->layers) return model_error(m, "out of memory");
    for (int il = 0; il < m->hp.n_layer; il++) {
        thimble_layer *L = &m->layers[il];
        L->n_kv_head = kv_heads(m, il);
        L->attn_norm   = model_tensor(m, il, "attn_norm.weight", 1, d, -1, -1);
        L->attn_norm_b = model_tensor(m, il, "attn_norm.bias", 0, d, -1, -1);
        L->ffn_norm    = model_tensor(m, il, "ffn_norm.weight", 1, d, -1, -1);
        L->ffn_norm_b  = model_tensor(m, il, "ffn_norm.bias", 0, d, -1, -1);
        if (!L->attn_norm || !L->ffn_norm) return -1;
        L->mixer = choose_mixer(m, il);
        L->ffn   = choose_ffn(m, il);
        if (L->mixer->load(m, L, il) != 0 || L->ffn->load(m, L, il) != 0) return -1;
    }
    return 0;
}

/* ---- public ----------------------------------------------------------- */

thimble_model *thimble_model_load(const char *path, char *err, size_t err_len) {
    char           dummy[8];
    thimble_model *m = calloc(1, sizeof(*m));
    if (!err || !err_len) err = dummy, err_len = sizeof(dummy);
    err[0] = '\0';
    if (!m) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    m->err = err, m->err_len = err_len;
    quant_init();
    if (gguf_open(path, &m->gguf, err, err_len) != 0) goto fail;

    gguf_str s;
    if (!gguf_get_str(&m->gguf, "general.architecture", &s) || s.n >= sizeof(m->arch)) {
        model_error(m, "%s: no general.architecture", path);
        goto fail;
    }
    memcpy(m->arch, s.s, s.n);
    if (strcmp(m->arch, "thimble") != 0 && strcmp(m->arch, "thimble-hybrid") != 0) {
        model_error(m, "unsupported architecture '%s' (the Thimble engine runs thimble and "
                       "thimble-hybrid GGUFs; export with --target thimble)", m->arch);
        goto fail;
    }
    if (gguf_get_str(&m->gguf, "general.name", &s)) {
        size_t n = s.n < sizeof(m->name) - 1 ? s.n : sizeof(m->name) - 1;
        memcpy(m->name, s.s, n);
    }
    if (load_hparams(m) != 0) goto fail;
    if (tokenizer_load(&m->tok, &m->gguf, err, err_len) != 0) goto fail;

    const int d = m->hp.n_embd;
    const gguf_tensor *te = gguf_find_tensor(&m->gguf, "token_embd.weight");
    if (!te) { model_error(m, "missing tensor token_embd.weight"); goto fail; }
    m->hp.n_vocab = (int) te->ne[1];
    m->tok_embd   = model_weight(m, -1, "token_embd.weight", 1, d, -1, -1);
    m->pos_embd   = model_tensor(m, -1, "position_embd.weight", 0, d, -1, -1);
    m->out_norm   = model_tensor(m, -1, "output_norm.weight", 1, d, -1, -1);
    m->out_norm_b = model_tensor(m, -1, "output_norm.bias", 0, d, -1, -1);
    m->output     = model_weight(m, -1, "output.weight", 1, d, m->hp.n_vocab, -1);
    if (!m->tok_embd.data || !m->out_norm || !m->output.data) goto fail;
    m->hp.layer_norm  = m->out_norm_b != NULL;
    m->hp.learned_pos = m->pos_embd != NULL;
    if (m->hp.copy_head) {
        /* F32 on purpose: the exporter leaves these unquantized because the
         * gate scales the whole pointer distribution. */
        m->copy_q      = model_tensor(m, -1, "copy_q.weight", 1, d, m->hp.copy_dim, -1);
        m->copy_k      = model_tensor(m, -1, "copy_k.weight", 1, d, m->hp.copy_dim, -1);
        m->copy_gate_w = model_tensor(m, -1, "copy_gate.weight", 1, d, 1, -1);
        m->copy_gate_b = model_tensor(m, -1, "copy_gate.bias", 1, 1, -1, -1);
        if (!m->copy_q || !m->copy_k || !m->copy_gate_w || !m->copy_gate_b) goto fail;
    }
    if (m->pos_embd) {
        const gguf_tensor *pe = gguf_find_tensor(&m->gguf, "position_embd.weight");
        m->hp.n_ctx_train = (int) pe->ne[1];
    }
    if (m->tok.n_vocab > m->hp.n_vocab) {
        model_error(m, "tokenizer has %d tokens but the model only %d", m->tok.n_vocab, m->hp.n_vocab);
        goto fail;
    }
    if (load_layers(m) != 0) goto fail;
    m->err = NULL, m->err_len = 0;
    return m;

fail:
    if (!err[0]) snprintf(err, err_len, "failed to load %s", path);
    m->err = NULL;
    thimble_model_free(m);
    return NULL;
}

void thimble_model_free(thimble_model *m) {
    if (!m) return;
    tokenizer_free(&m->tok);
    free(m->layers);
    gguf_close(&m->gguf);
    free(m);
}

int thimble_n_vocab(const thimble_model *m) { return m->hp.n_vocab; }
int thimble_n_ctx_train(const thimble_model *m) { return m->hp.n_ctx_train; }
int thimble_eos_id(const thimble_model *m) { return m->tok.eos; }
int thimble_has_thinking(const thimble_model *m) { return m->hp.thinking && m->tok.think >= 0; }
const char *thimble_arch(const thimble_model *m) { return m->arch; }

void thimble_describe(const thimble_model *m, char *buf, size_t len) {
    const thimble_hparams *hp = &m->hp;
    size_t w = 0;
    w += (size_t) snprintf(buf + w, w < len ? len - w : 0,
        "name: %s\narch: %s\nvocab: %d  context: %d  d_model: %d  layers: %d  heads: %d  head_dim: %d\n"
        "norm: %s  positions: %s  ffn: %s%s  thinking: %s  copy head: %s\nlayers:",
        m->name[0] ? m->name : "(unnamed)", m->arch, hp->n_vocab, hp->n_ctx_train, hp->n_embd,
        hp->n_layer, hp->n_head, hp->head_dim, hp->layer_norm ? "layernorm" : "rmsnorm",
        hp->learned_pos ? "learned" : "rope", hp->n_expert ? "moe" : "dense",
        hp->n_expert ? "" : (m->layers[0].ffn_gate.data ? " swiglu" : " gelu"),
        thimble_has_thinking(m) ? "yes" : "no",
        hp->copy_head ? "yes" : "no");
    for (int il = 0; il < hp->n_layer && w < len; il++) {
        const thimble_layer *L = &m->layers[il];
        if (L->mixer == &MIXER_SHORTCONV)
            w += (size_t) snprintf(buf + w, len - w, " conv");
        else
            w += (size_t) snprintf(buf + w, w < len ? len - w : 0, " attn(kv=%d%s%s)", L->n_kv_head,
                                   L->q_norm ? ",qknorm" : "", L->wgate.data ? ",gate" : "");
    }
    if (hp->n_expert && w < len)
        w += (size_t) snprintf(buf + w, len - w, "\nexperts: %d (top-%d, d_ff %d)", hp->n_expert,
                               hp->n_expert_used, hp->n_ff_exp);
    /* weight formats in use */
    int seen[64] = { 0 };
    if (w < len) w += (size_t) snprintf(buf + w, len - w, "\nweights:");
    for (uint64_t i = 0; i < m->gguf.n_tensors && w < len; i++) {
        int t = (int) m->gguf.tensors[i].type;
        if (t >= 0 && t < 64 && !seen[t]++) w += (size_t) snprintf(buf + w, len - w, " %s", quant_name(t));
    }
}
