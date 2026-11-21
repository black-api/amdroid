/* thimble serve: model loading, routing, auth, metadata endpoints, metrics, web UI.
 *
 *   POST /v1/chat/completions  /v1/completions   (also without the /v1 prefix)
 *   POST /tokenize  /detokenize
 *   GET  /v1/models  /props  /metrics  /health  /
 */
#include "server.h"
#include "server_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char WEB_UI[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Thimble</title><style>"
    ":root{--bg:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--acc:#0a66c2;--you:#eef4fb}"
    "@media(prefers-color-scheme:dark){:root{--bg:#161618;--fg:#f2f2f2;--mut:#9a9aa0;--line:#38383c;--acc:#5aa2ff;--you:#1f2a38}}"
    "body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}"
    "main{max-width:760px;margin:0 auto;padding:16px}header{display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
    "h1{font-size:18px;margin:0 auto 0 0}select,input,button,textarea{font:inherit;color:inherit;background:var(--bg);"
    "border:1px solid var(--line);border-radius:8px;padding:6px 8px}button{background:var(--acc);color:#fff;border:0;cursor:pointer}"
    "#log{margin:16px 0}.m{padding:8px 12px;border-radius:10px;margin:8px 0;white-space:pre-wrap}.you{background:var(--you)}"
    ".think{color:var(--mut);font-style:italic;border-left:3px solid var(--line);padding-left:10px;white-space:pre-wrap}"
    ".meta{color:var(--mut);font-size:12px}form{display:flex;gap:8px}textarea{flex:1;min-height:44px}"
    "label{color:var(--mut);font-size:13px}input[type=number]{width:64px}"
    "</style></head><body><main><header><h1>Thimble</h1><select id=model></select>"
    "<label><input type=checkbox id=think checked> think</label>"
    "<label>temp <input type=number id=temp step=0.1 min=0 value=0.8></label>"
    "<label>max <input type=number id=max min=1 value=128></label><button type=button id=clear>New chat</button></header>"
    "<div id=log></div><form id=f><textarea id=q placeholder='Message' autofocus></textarea><button>Send</button></form></main>"
    "<script>let msgs=[];const $=i=>document.getElementById(i),log=$('log');"
    "fetch('v1/models').then(r=>r.json()).then(j=>{for(const m of j.data){const o=document.createElement('option');"
    "o.value=m.id;o.textContent=m.id+(m.thinking?' (think)':'');$('model').appendChild(o)}});"
    "function el(c,t){const d=document.createElement('div');d.className=c;d.textContent=t||'';log.appendChild(d);return d}"
    "$('clear').onclick=()=>{msgs=[];log.innerHTML=''};"
    "$('q').onkeydown=e=>{if(e.key=='Enter'&&!e.shiftKey){e.preventDefault();$('f').requestSubmit()}};"
    "$('f').onsubmit=async e=>{e.preventDefault();const text=$('q').value.trim();if(!text)return;$('q').value='';"
    "el('m you',text);msgs.push({role:'user',content:text});const th=el('think'),out=el('m'),meta=el('meta');th.hidden=true;"
    "const r=await fetch('v1/chat/completions',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({"
    "model:$('model').value,messages:msgs,stream:true,max_tokens:+$('max').value,temperature:+$('temp').value,"
    "enable_thinking:$('think').checked,stream_options:{include_usage:true}})});"
    "if(!r.ok){out.textContent=(await r.json()).error.message;return}const rd=r.body.getReader(),dec=new TextDecoder();let buf='';"
    "for(;;){const{done,value}=await rd.read();if(done)break;buf+=dec.decode(value,{stream:true});let i;"
    "while((i=buf.indexOf('\\n\\n'))>=0){const line=buf.slice(0,i);buf=buf.slice(i+2);if(!line.startsWith('data: '))continue;"
    "const d=line.slice(6);if(d=='[DONE]')continue;const j=JSON.parse(d);if(j.usage){meta.textContent=j.usage.completion_tokens+"
    "' tokens, '+j.timings.predicted_per_second+' tok/s'+(j.usage.prompt_tokens_details.cached_tokens?', '+"
    "j.usage.prompt_tokens_details.cached_tokens+' cached':'');continue}const dl=j.choices[0].delta;"
    "if(dl.reasoning_content){th.hidden=false;th.textContent+=dl.reasoning_content}if(dl.content)out.textContent+=dl.content}}"
    "msgs.push({role:'assistant',content:out.textContent.trim()})};</script></body></html>";

/* ---- endpoints ------------------------------------------------------------------- */

static void send_models(srv *s, http_request *req) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"object\":\"list\",\"data\":[");
    for (int i = 0; i < s->n_models; i++) {
        const srv_model *m = &s->models[i];
        sb_puts(&b, i ? ",{\"id\":" : "{\"id\":");
        sb_json_str(&b, m->alias, strlen(m->alias));
        sb_printf(&b, ",\"object\":\"model\",\"created\":%ld,\"owned_by\":\"thimble\",\"arch\":\"%s\","
                      "\"thinking\":%s,\"context_length\":%d,\"vocab_size\":%d}",
                  s->started, thimble_arch(m->model), thimble_has_thinking(m->model) ? "true" : "false", m->n_ctx,
                  thimble_n_vocab(m->model));
    }
    sb_puts(&b, "]}");
    http_send_json(req, 200, b.s);
    sb_free(&b);
}

static void send_props(srv *s, http_request *req) {
    char *cfg = server_config_json(s->cfg);
    sbuf  b;
    sb_init(&b);
    sb_printf(&b, "{\"engine\":\"thimble\",\"uptime_s\":%ld,\"settings\":%s,\"models\":[", (long) time(NULL) - s->started, cfg);
    free(cfg);
    for (int i = 0; i < s->n_models; i++) {
        char info[4096];
        thimble_describe(s->models[i].model, info, sizeof(info));
        sb_puts(&b, i ? ",{\"id\":" : "{\"id\":");
        sb_json_str(&b, s->models[i].alias, strlen(s->models[i].alias));
        sb_printf(&b, ",\"context_length\":%d,\"describe\":", s->models[i].n_ctx);
        sb_json_str(&b, info, strlen(info));
        sb_puts(&b, "}");
    }
    sb_puts(&b, "]}");
    http_send_json(req, 200, b.s);
    sb_free(&b);
}

static void send_metrics(srv *s, http_request *req) {
    pthread_mutex_lock(&s->m.mu);
    srv_metrics m = s->m;
    pthread_mutex_unlock(&s->m.mu);
    sbuf b;
    sb_init(&b);
#define METRIC(name, type, help, fmt, val) \
    sb_printf(&b, "# HELP thimble_" name " " help "\n# TYPE thimble_" name " " type "\nthimble_" name " " fmt "\n", val)
    METRIC("requests_total", "counter", "HTTP requests handled", "%llu", (unsigned long long) m.requests);
    METRIC("errors_total", "counter", "requests answered with an error", "%llu", (unsigned long long) m.errors);
    METRIC("generations_total", "counter", "completions generated (choices)", "%llu", (unsigned long long) m.generations);
    METRIC("prompt_tokens_total", "counter", "prompt tokens", "%llu", (unsigned long long) m.prompt_tokens);
    METRIC("prompt_tokens_cached_total", "counter", "prompt tokens reused from the cache", "%llu", (unsigned long long) m.cached_tokens);
    METRIC("completion_tokens_total", "counter", "generated tokens", "%llu", (unsigned long long) m.completion_tokens);
    METRIC("dropped_messages_total", "counter", "chat turns dropped to fit the context", "%llu", (unsigned long long) m.dropped_messages);
    METRIC("prompt_seconds_total", "counter", "time spent on prompts", "%.3f", m.prompt_seconds);
    METRIC("generation_seconds_total", "counter", "time spent generating", "%.3f", m.gen_seconds);
    METRIC("requests_active", "gauge", "requests in progress", "%d", m.active);
    METRIC("workers", "gauge", "worker threads", "%d", s->cfg->parallel);
#undef METRIC
    http_send(req, 200, "text/plain; version=0.0.4", b.s, b.n);
    sb_free(&b);
}

static void handle_tokenize(srv *s, http_request *req, const json *body) {
    char err[512];
    const srv_model *sm = srv_pick_model(s, body, err, sizeof(err));
    if (!sm) { http_send_error(req, 404, err); return; }
    const char *text = json_str(json_get(body, "content"), json_str(json_get(body, "text"), NULL));
    if (!text) { http_send_error(req, 400, "content must be a string"); return; }
    int with_pieces = json_bool(json_get(body, "with_pieces"), 0);
    int n = thimble_tokenize(sm->model, text, NULL, 0);
    int *ids = malloc(sizeof(int) * (size_t) (n + 1));
    thimble_tokenize(sm->model, text, ids, n);
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"tokens\":[");
    for (int i = 0; i < n; i++) {
        if (!with_pieces) { sb_printf(&b, i ? ",%d" : "%d", ids[i]); continue; }
        char piece[256];
        int  len = thimble_token_piece(sm->model, ids[i], piece, (int) sizeof(piece), 1);
        sb_printf(&b, "%s{\"id\":%d,\"piece\":", i ? "," : "", ids[i]);
        sb_json_str(&b, piece, (size_t) (len < 256 ? len : 256));
        sb_puts(&b, "}");
    }
    sb_printf(&b, "],\"count\":%d}", n);
    http_send_json(req, 200, b.s);
    sb_free(&b);
    free(ids);
}

static void handle_detokenize(srv *s, http_request *req, const json *body) {
    char err[512];
    const srv_model *sm = srv_pick_model(s, body, err, sizeof(err));
    if (!sm) { http_send_error(req, 404, err); return; }
    const json *toks = json_get(body, "tokens");
    if (!toks || toks->type != J_ARR) { http_send_error(req, 400, "tokens must be an array of ids"); return; }
    int with_special = json_bool(json_get(body, "with_special"), 0);
    sbuf text;
    sb_init(&text);
    for (const json *t = toks->child; t; t = t->next) {
        int id = (int) json_num(t, -1);
        if (id < 0 || id >= thimble_n_vocab(sm->model)) { sb_free(&text); http_send_error(req, 400, "token id out of range"); return; }
        char piece[256];
        int  len = thimble_token_piece(sm->model, id, piece, (int) sizeof(piece), with_special);
        sb_putn(&text, piece, (size_t) (len < 256 ? len : 256));
    }
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"content\":");
    sb_json_str(&b, text.s ? text.s : "", text.n);
    sb_puts(&b, "}");
    http_send_json(req, 200, b.s);
    sb_free(&b);
    sb_free(&text);
}

/* ---- routing ------------------------------------------------------------------------ */

static int authorized(const srv *s, const http_request *req) {
    if (!s->cfg->api_key[0]) return 1;
    const char *a = req->authorization;
    return !strncmp(a, "Bearer ", 7) && !strcmp(a + 7, s->cfg->api_key);
}

static int path_is(const char *path, const char *name) {   /* "/v1/x" or "/x" */
    return !strcmp(path, name) || (!strncmp(path, "/v1", 3) && !strcmp(path + 3, name));
}

static void route(srv *s, http_request *req) {
    const char *p = req->path;
    if (!strcmp(req->method, "OPTIONS")) { http_send(req, 204, "text/plain", "", 0); return; }
    if (!strcmp(req->method, "GET") && !strcmp(p, "/health")) {
        char js[128];
        snprintf(js, sizeof(js), "{\"status\":\"ok\",\"models\":%d,\"workers\":%d,\"voice\":%s}", s->n_models, s->cfg->parallel, s->voice ? "true" : "false");
        http_send_json(req, 200, js);
        return;
    }
    if (!strcmp(req->method, "GET") && !strcmp(p, "/") && s->cfg->webui) {
        http_send(req, 200, "text/html; charset=utf-8", WEB_UI, sizeof(WEB_UI) - 1);
        return;
    }
    if (!authorized(s, req)) { http_send_error(req, 401, "missing or invalid API key"); return; }

    if (!strcmp(req->method, "GET")) {
        if (path_is(p, "/models")) send_models(s, req);
        else if (!strcmp(p, "/props")) send_props(s, req);
        else if (!strcmp(p, "/metrics") && s->cfg->metrics) send_metrics(s, req);
        else http_send_error(req, 404, "not found");
        return;
    }
    if (strcmp(req->method, "POST")) { http_send_error(req, 405, "method not allowed"); return; }

    int chat = path_is(p, "/chat/completions"), completion = path_is(p, "/completions");
    int tok = !strcmp(p, "/tokenize"), detok = !strcmp(p, "/detokenize"), speech = path_is(p, "/audio/speech");
    if (!chat && !completion && !tok && !detok && !speech) { http_send_error(req, 404, "not found"); return; }
    char  err[256];
    json *body = json_parse(req->body ? req->body : "", req->body_len, err, sizeof(err));
    if (!body || body->type != J_OBJ) {
        http_send_error(req, 400, body ? "body must be a JSON object" : err);
        json_free(body);
        return;
    }
    if (speech) srv_handle_speech(s, req, body);
    else if (chat || completion) srv_handle_generation(s, req, body, chat);
    else if (tok) handle_tokenize(s, req, body);
    else handle_detokenize(s, req, body);
    json_free(body);
}

static void handle(http_request *req, void *user) {
    srv   *s  = user;
    double t0 = now_seconds();
    pthread_mutex_lock(&s->m.mu);
    s->m.requests++, s->m.active++;
    pthread_mutex_unlock(&s->m.mu);
    route(s, req);
    pthread_mutex_lock(&s->m.mu);
    s->m.active--;
    pthread_mutex_unlock(&s->m.mu);
    if (s->cfg->log_level >= 1)
        fprintf(stderr, "[w%d] %s %s %s %.1f ms\n", req->worker, req->remote, req->method, req->path,
                (now_seconds() - t0) * 1e3);
}

/* ---- startup -------------------------------------------------------------------------- */

static int load_models(srv *s, char *err, size_t err_len) {
    const server_config *c = s->cfg;
    for (int i = 0; i < c->n_models; i++) {
        srv_model *sm = &s->models[s->n_models];
        sm->model = thimble_model_load(c->models[i].path, err, err_len);
        if (!sm->model) return -1;
        if (c->models[i].alias[0]) {
            snprintf(sm->alias, sizeof(sm->alias), "%s", c->models[i].alias);
        } else {   /* general.name, else the file name */
            char info[4096];
            thimble_describe(sm->model, info, sizeof(info));
            const char *nm = strstr(info, "name: ");
            snprintf(sm->alias, sizeof(sm->alias), "%s", nm ? nm + 6 : c->models[i].path);
            char *nl = strchr(sm->alias, '\n');
            if (nl) *nl = '\0';
            if (!strcmp(sm->alias, "(unnamed)")) {
                const char *base = strrchr(c->models[i].path, '/');
                snprintf(sm->alias, sizeof(sm->alias), "%s", base ? base + 1 : c->models[i].path);
            }
        }
        for (int j = 0; j < s->n_models; j++)
            if (!strcmp(s->models[j].alias, sm->alias)) {
                snprintf(err, err_len, "duplicate model name '%s' (use --model PATH=ALIAS)", sm->alias);
                return -1;
            }
        sm->slots = calloc((size_t) c->parallel, sizeof(srv_slot));
        for (int w = 0; w < c->parallel; w++) {
            sm->slots[w].ctx = thimble_ctx_new(sm->model, c->n_ctx);
            if (!sm->slots[w].ctx) { snprintf(err, err_len, "cannot allocate context"); return -1; }
            sm->n_ctx = thimble_ctx_n_ctx(sm->slots[w].ctx);
            sm->slots[w].cached = calloc((size_t) sm->n_ctx + 1, sizeof(int));
        }
        s->n_models++;
        if (c->log_level >= 1)
            fprintf(stderr, "loaded %s as '%s' (%s, ctx %d%s)\n", c->models[i].path, sm->alias,
                    thimble_arch(sm->model), sm->n_ctx, thimble_has_thinking(sm->model) ? ", think mode" : "");
    }
    return 0;
}

int server_main(int argc, char **argv) {
    static server_config cfg;
    char err[512];
    server_config_defaults(&cfg);
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { server_config_usage(); return 0; }
    if (server_config_parse(&cfg, argc, argv, err, sizeof(err))) {
        fprintf(stderr, "error: %s\n", err);
        server_config_usage();
        return 2;
    }
    static srv s;
    s.cfg = &cfg;
    s.started = (long) time(NULL);
    pthread_mutex_init(&s.m.mu, NULL);
    if (load_models(&s, err, sizeof(err))) { fprintf(stderr, "error: %s\n", err); return 1; }
    pthread_mutex_init(&s.voice_mu, NULL);
    srv_load_voice(&s);
    if (cfg.api_key[0] && cfg.log_level >= 1) fprintf(stderr, "API key required (Authorization: Bearer ...)\n");
    http_options opt = { cfg.host, cfg.port, cfg.parallel, cfg.queue_size, cfg.keepalive_s, cfg.timeout_s,
                         cfg.max_body, cfg.cors[0] ? cfg.cors : NULL };
    return http_serve(&opt, handle, &s) ? 1 : 0;
}
