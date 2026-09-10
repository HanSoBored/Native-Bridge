#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <limits.h>

#define CONFIG_MAX_BLOCKED 256
#define CONFIG_ENTRY_MAX_LEN 255
#define CONFIG_FILE_MAX_SIZE (64 * 1024)

typedef struct nb_config {
    char *blocked[CONFIG_MAX_BLOCKED];
    size_t count;
    char source_path[PATH_MAX];
} nb_config_t;

int  config_parse_json(const char *buf, size_t len, nb_config_t *cfg,
                       char *errbuf, size_t errbufsz);
int  config_load_file(const char *path, nb_config_t *cfg,
                      char *errbuf, size_t errbufsz);
const char *config_discover(const char *env_path, char *out, size_t outsz);
int  config_is_blocked(const nb_config_t *cfg, const char *cmd);
void config_defaults(nb_config_t *cfg);
void config_clear(nb_config_t *cfg);

#endif /* CONFIG_H */
