/* sticktime.txt: "key = value" settings on the FAT boot partition, editable from a PC.
 *
 * Format (shared with image/rootfs/usr/lib/sticktime/settings.py, which must parse it the same way):
 *  - UTF-8, an optional BOM, LF or CRLF line ends.
 *  - Blank lines and lines whose first non-space character is '#' are comments.
 *  - "key = value": the key is trimmed and lower-cased; the value is trimmed.
 *  - In an unquoted value, a '#' that follows a space or tab starts a comment.
 *  - A value in double quotes keeps everything inside; \" and \\ are escapes there.
 *  - A later line with the same key wins. Lines without '=' are ignored (a warning).
 * Saving keeps every line, comment and the order; it rewrites only the values that changed
 * (keeping their inline comment) and appends new keys at the end. Values that need it are
 * written in quotes. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct settings settings_t;

/* The documented default file, written when there is none. */
extern const char SETTINGS_TEMPLATE[];

settings_t *settings_new(void);                         /* empty */
settings_t *settings_parse(const char *text, size_t len);
/* Loads path; when it is missing returns the template's settings (exists=false). NULL only on OOM. */
settings_t *settings_load(const char *path, bool *exists);
void settings_free(settings_t *s);

/* raw value of a key (as parsed: unquoted, unescaped), or NULL when the key is not set */
const char *settings_raw(const settings_t *s, const char *key);
/* value or the built-in default of a known key ("" for unknown keys without a value) */
const char *settings_get(const settings_t *s, const char *key);
int    settings_get_int(const settings_t *s, const char *key, int def, int min, int max);
double settings_get_num(const settings_t *s, const char *key, double def, double min, double max);
bool   settings_get_bool(const settings_t *s, const char *key, bool def);  /* on/off yes/no true/false 1/0 */

/* Set a value (NULL removes the key's lines). Returns false for an invalid key name. */
bool settings_set(settings_t *s, const char *key, const char *value);
/* The file text. Caller frees. */
char *settings_serialize(const settings_t *s, size_t *len);
int   settings_save(const settings_t *s, const char *path);   /* atomic: 0 ok, -1 errno */

/* Iterate the effective key/values in file order (each key once, last value wins). */
typedef void (*settings_cb)(void *ud, const char *key, const char *value);
void settings_each(const settings_t *s, settings_cb cb, void *ud);
/* Warnings from parsing / validation ("line 12: no '='"), one per line, "" when none. */
const char *settings_warnings(const settings_t *s);

/* ---- known keys, their defaults and validation ---- */
typedef enum { SK_STRING, SK_ENUM, SK_INT, SK_NUM, SK_BOOL, SK_SECRET } settings_kind_t;
typedef struct {
  const char *key;
  settings_kind_t kind;
  const char *def;          /* default value text */
  const char *choices;      /* SK_ENUM: "av|hdmi"; SK_INT/SK_NUM: "min|max" */
  bool live;                /* applied without a restart of the host */
  bool reboot;              /* needs a reboot (config.txt / cmdline.txt) */
  const char *help;         /* one line, for the web UI */
} settings_key_t;
extern const settings_key_t SETTINGS_KEYS[];
extern const int SETTINGS_NKEYS;
const settings_key_t *settings_key(const char *key);
/* Validate a value for a known key: NULL if fine, else a message. Unknown keys are accepted. */
const char *settings_validate(const char *key, const char *value);

#endif
