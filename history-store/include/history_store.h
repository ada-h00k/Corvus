#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H

/*
 * Encrypted chat-history store (Rust, XChaCha20-Poly1305 + Argon2id).
 * All records are JSON objects identified by their "t" tag (the store knows
 * exactly these; an unknown tag is an error):
 *   {"t":"m","c":<conv>,"ts":<ms>,"s":<sender>,"x":<text>,"o":<outgoing>,"au":<author>,"d":{...}}
 *   {"t":"n","c":<conv>,"n":<display name>}          learnt profile name
 *   {"t":"p","c":<conv>,"n":<nickname>}              local nickname
 *   {"t":"d","c":<conv>}                             conversation deleted
 *   {"t":"u","c":<conv>,"ts":<ms>,"au":<author>,"d":{...}}   message extras update
 *   {"t":"r","c":<conv>,"ts":<ms>,"au":<author>,"by":<key>,"e":<emoji>}   reaction
 *   {"t":"a","c":<conv>,"v":<archived>,"s":<signal-cli archived>}
 *   {"t":"k","c":<conv>,"v":<muted>}
 *   {"t":"e","c":<conv>,"v":<seconds>}               disappearing-message timer
 *   {"t":"pv","c":<conv>,"ts":<ms>,"au":<author>,"by":<key>,"oi":[<option>],"vc":<n>,"vt":<ms>}   poll vote
 *   {"t":"pi","c":<conv>,"ts":<ms>,"au":<author>,"by":<key>,"at":<ms>,"d":<sec>,"rm":<unpin>}     pin
 *   {"t":"b","c":<conv>,"v":<blocked>}
 *   {"t":"j","c":<conv>}                             message request declined
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
