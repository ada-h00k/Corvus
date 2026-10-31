#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H

/*
 * Encrypted chat-history store (Rust, XChaCha20-Poly1305 + Argon2id).
 * All records are JSON objects:
 *   {"t":"m","c":<conversation key>,"ts":<ms>,"s":<sender>,"x":<text>,"o":<outgoing bool>}
 *   {"t":"n","c":<conversation key>,"n":<display name>}
 *   {"t":"d","c":<conversation key>}
 * Use a handle from one thread at a time.
 */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SC_OK = 0,
    SC_ERR_WRONG_PASSPHRASE = 1,
    SC_ERR_CORRUPT = 2,
    SC_ERR_EXISTS = 3,
    SC_ERR_IO = 4,
    SC_ERR_CRYPTO = 5,
    SC_ERR_ARG = 6,
    SC_ERR_PANIC = 7
};

typedef struct ScStore ScStore;

int sc_store_exists(const char *path);
ScStore *sc_store_create(const char *path, const char *passphrase, int *err);
/* On success *records_json = JSON array of all records; free with sc_string_free. */
ScStore *sc_store_open(const char *path, const char *passphrase, char **records_json, int *err);
int sc_store_append(ScStore *s, const char *record_json);
/* Append a JSON array of records with one write + one fsync (same on-disk format as repeated appends). */
int sc_store_append_batch(ScStore *s, const char *records_json_array);
int sc_store_rewrite(ScStore *s, const char *records_json_array);
int sc_store_change_passphrase(ScStore *s, const char *old_pass, const char *new_pass);
void sc_store_close(ScStore *s);
void sc_string_free(char *s);

#ifdef __cplusplus
}
#endif

#endif
