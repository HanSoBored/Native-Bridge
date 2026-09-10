#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "config.h"

#define JSMN_STATIC
#include "jsmn.h"

#define CONFIG_TOKEN_CAP 1024

/* NULL-terminated sentinel list; empty by default (nothing is blocked). */
static const char *const BUILTIN_BLOCKED[] = { NULL };

static int json_eq(const char *json, const jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, (size_t)(tok->end - tok->start)) == 0) {
        return 0;
    }
    return -1;
}

/* Returns the index of the token after the subtree rooted at tokens[idx].
 * Only called on well-formed token trees (jsmn_parse returned >= 0). */
static int skip_subtree(const jsmntok_t *tokens, int idx) {
    const jsmntok_t *t = &tokens[idx];
    int end = idx + 1;
    if (t->type == JSMN_OBJECT) {
        for (int i = 0; i < t->size; i++) {
            end = skip_subtree(tokens, end);
            end = skip_subtree(tokens, end);
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int i = 0; i < t->size; i++) {
            end = skip_subtree(tokens, end);
        }
    }
    return end;
}

/* Copies a JSON string token into `out`, decoding standard escapes.
 * Returns the number of characters written (without the NUL terminator). */
static size_t unescape_json_string(const char *json, const jsmntok_t *tok,
                                   char *out, size_t max) {
    size_t out_idx = 0;
    for (int j = tok->start; j < tok->end && out_idx < max - 1; j++) {
        if (json[j] == '\\' && j + 1 < tok->end) {
            j++;
            switch (json[j]) {
                case '"': out[out_idx++] = '"'; break;
                case '\\': out[out_idx++] = '\\'; break;
                case '/': out[out_idx++] = '/'; break;
                case 'b': out[out_idx++] = '\b'; break;
                case 'f': out[out_idx++] = '\f'; break;
                case 'n': out[out_idx++] = '\n'; break;
                case 'r': out[out_idx++] = '\r'; break;
                case 't': out[out_idx++] = '\t'; break;
                default: out[out_idx++] = '\\'; out[out_idx++] = json[j]; break;
            }
        } else {
            out[out_idx++] = json[j];
        }
    }
    out[out_idx] = '\0';
    return out_idx;
}

void config_clear(nb_config_t *cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < cfg->count; i++) {
        free(cfg->blocked[i]);
        cfg->blocked[i] = NULL;
    }
    cfg->count = 0;
    cfg->source_path[0] = '\0';
}

void config_defaults(nb_config_t *cfg) {
    if (!cfg) return;
    config_clear(cfg);
    for (size_t i = 0; BUILTIN_BLOCKED[i]; i++) {
        cfg->blocked[cfg->count] = strdup(BUILTIN_BLOCKED[i]);
        if (cfg->blocked[cfg->count]) cfg->count++;
    }
    snprintf(cfg->source_path, sizeof(cfg->source_path), "<built-in defaults>");
}

int config_is_blocked(const nb_config_t *cfg, const char *cmd) {
    if (!cfg || !cmd) return 1;
    for (size_t i = 0; i < cfg->count; i++) {
        const char *entry = cfg->blocked[i];
        if (!entry) continue;
        if (strcmp(cmd, entry) == 0) return 1;
        const char *last_slash = strrchr(cmd, '/');
        if (last_slash && strcmp(last_slash + 1, entry) == 0) return 1;
    }
    return 0;
}

int config_parse_json(const char *buf, size_t len, nb_config_t *cfg,
                      char *errbuf, size_t errbufsz) {
    if (errbufsz > 0) errbuf[0] = '\0';
    config_clear(cfg);

    jsmn_parser p;
    jsmntok_t tokens[CONFIG_TOKEN_CAP];
    jsmn_init(&p);
    int r = jsmn_parse(&p, buf, len, tokens, CONFIG_TOKEN_CAP);
    if (r < 0) {
        snprintf(errbuf, errbufsz, "malformed JSON (jsmn error %d)", r);
        return -1;
    }
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        snprintf(errbuf, errbufsz, "config root must be a JSON object");
        return -1;
    }

    int idx = 1;
    int saw_blocked = 0;
    for (int i = 0; i < tokens[0].size; i++) {
        if (idx >= r) break;
        const jsmntok_t *key = &tokens[idx];
        idx = skip_subtree(tokens, idx);
        if (key->type != JSMN_STRING) {
            snprintf(errbuf, errbufsz, "config keys must be strings");
            return -1;
        }
        if (idx >= r) break;
        const jsmntok_t *val = &tokens[idx];
        int val_end = skip_subtree(tokens, idx);

        if (json_eq(buf, key, "blocked_commands") == 0) {
            saw_blocked = 1;
            if (val->type != JSMN_ARRAY) {
                snprintf(errbuf, errbufsz, "'blocked_commands' must be an array");
                return -1;
            }
            int child = idx + 1;
            for (int j = 0; j < val->size; j++) {
                if (child >= r) break;
                const jsmntok_t *elem = &tokens[child];
                child = skip_subtree(tokens, child);
                if (elem->type != JSMN_STRING) {
                    snprintf(errbuf, errbufsz,
                             "'blocked_commands[%d]' must be a string", j);
                    return -1;
                }
                size_t elen = (size_t)(elem->end - elem->start);
                if (elen == 0 || elen > (size_t)CONFIG_ENTRY_MAX_LEN) {
                    snprintf(errbuf, errbufsz,
                             "'blocked_commands[%d]' invalid length", j);
                    return -1;
                }
                if (cfg->count >= (size_t)CONFIG_MAX_BLOCKED) {
                    snprintf(errbuf, errbufsz,
                             "too many blocked commands (max %d)", CONFIG_MAX_BLOCKED);
                    return -1;
                }
                char entry[CONFIG_ENTRY_MAX_LEN + 1];
                unescape_json_string(buf, elem, entry, sizeof(entry));
                int dup = 0;
                for (size_t k = 0; k < cfg->count; k++) {
                    if (cfg->blocked[k] && strcmp(cfg->blocked[k], entry) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    cfg->blocked[cfg->count] = strdup(entry);
                    if (!cfg->blocked[cfg->count]) {
                        snprintf(errbuf, errbufsz, "out of memory");
                        return -1;
                    }
                    cfg->count++;
                }
            }
        }
        idx = val_end;
    }

    if (!saw_blocked) {
        snprintf(errbuf, errbufsz, "config missing 'blocked_commands' key");
        return -1;
    }
    return 0;
}

int config_load_file(const char *path, nb_config_t *cfg,
                     char *errbuf, size_t errbufsz) {
    if (errbufsz > 0) errbuf[0] = '\0';
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(errbuf, errbufsz, "cannot open '%s': %s", path, strerror(errno));
        return -1;
    }
    char *buf = malloc(CONFIG_FILE_MAX_SIZE + 1);
    if (!buf) {
        fclose(fp);
        snprintf(errbuf, errbufsz, "out of memory");
        return -1;
    }
    size_t nread = fread(buf, 1, CONFIG_FILE_MAX_SIZE + 1, fp);
    fclose(fp);
    if (nread > (size_t)CONFIG_FILE_MAX_SIZE) {
        free(buf);
        snprintf(errbuf, errbufsz, "config file exceeds %d bytes", CONFIG_FILE_MAX_SIZE);
        return -1;
    }
    buf[nread] = '\0';

    int rc = config_parse_json(buf, nread, cfg, errbuf, errbufsz);
    free(buf);
    if (rc == 0) {
        snprintf(cfg->source_path, sizeof(cfg->source_path), "%s", path);
    }
    return rc;
}

const char *config_discover(const char *env_path, char *out, size_t outsz) {
    if (env_path && env_path[0]) return env_path;

    /* Primary location: ~/.config/native-bridge/blacklist.json (chroot side). */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        int n = snprintf(out, outsz, "%s/.config/native-bridge/blacklist.json", home);
        if (n < 0 || (size_t)n >= outsz) return NULL;
        return out;
    }
    return NULL;
}
