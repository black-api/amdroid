/* ThimbleLM chat template and tokenizer API wrappers.
 *   <|system|>..<|user|>..<|assistant|>..  then  <|assistant|>
 *   think mode on:  ... <|assistant|><|think|>
 *   think mode off: ... <|assistant|><|think|><|/think|>
 * Earlier assistant turns are rendered without their reasoning. */
#include "model.h"

#include <stdlib.h>
#include <string.h>

int thimble_tokenize(const thimble_model *m, const char *text, int *out, int max_out) {
    return tokenizer_encode(&m->tok, text, out, max_out);
}

int thimble_token_piece(const thimble_model *m, int id, char *buf, int len, int with_special) {
    return tokenizer_piece(&m->tok, id, buf, len, with_special);
}

char *thimble_render_chat(const thimble_model *m, const thimble_message *msgs, int n_msgs,
                          int thinking) {
    if (thinking == THIMBLE_THINK_DEFAULT) thinking = thimble_has_thinking(m) ? THIMBLE_THINK_ON : -2;
    else if (!thimble_has_thinking(m)) return NULL;   /* model was not trained with think mode */

    size_t cap = 64;
    for (int i = 0; i < n_msgs; i++) cap += strlen(msgs[i].content) + 16;
    char *s = malloc(cap);
    if (!s) return NULL;
    s[0] = '\0';
    for (int i = 0; i < n_msgs; i++) {
        const char *role = msgs[i].role;
        if (strcmp(role, "system") && strcmp(role, "user") && strcmp(role, "assistant")) {
            free(s);
            return NULL;
        }
        strcat(s, "<|");
        strcat(s, role);
        strcat(s, "|>");
        strcat(s, msgs[i].content);
    }
    strcat(s, "<|assistant|>");
    if (thinking == THIMBLE_THINK_ON)  strcat(s, "<|think|>");
    if (thinking == THIMBLE_THINK_OFF) strcat(s, "<|think|><|/think|>");
    return s;
}
