/*
 * Minimal bcrypt.h wrapper — adapts libxcrypt's crypt_gensalt_rn / crypt_r
 * to the classic bcrypt API expected by bcrypt_util.hpp.
 *
 * BCRYPT_HASHSIZE is set to 64 to match the traditional bcrypt hash length
 * (60 chars + NUL + margin).  The underlying libxcrypt CRYPT_OUTPUT_SIZE is
 * 384, so 64 is sufficient for bcrypt "$2b$..." hashes.
 */

#ifndef BCRYPT_H
#define BCRYPT_H

#define BCRYPT_HASHSIZE 64

#ifdef __cplusplus
extern "C" {
#endif

#include <crypt.h>
#include <string.h>

static inline int bcrypt_gensalt(int log_rounds, char *salt) {
    if (salt == NULL) return -1;
    char *result = crypt_gensalt_rn("$2b$", (unsigned long)log_rounds,
                                    NULL, 0,
                                    salt, CRYPT_GENSALT_OUTPUT_SIZE);
    return (result != NULL) ? 0 : -1;
}

static inline int bcrypt_hashpw(const char *pass, const char *salt, char *hash) {
    if (pass == NULL || salt == NULL || hash == NULL) return -1;
    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *result = crypt_r(pass, salt, &data);
    if (result == NULL) return -1;
    strncpy(hash, result, BCRYPT_HASHSIZE - 1);
    hash[BCRYPT_HASHSIZE - 1] = '\0';
    return 0;
}

static inline int bcrypt_checkpw(const char *pass, const char *hash) {
    if (pass == NULL || hash == NULL) return -1;
    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *result = crypt_r(pass, hash, &data);
    if (result == NULL) return -1;
    return (strcmp(result, hash) == 0) ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* BCRYPT_H */
