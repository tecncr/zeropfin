#include <stdlib.h>
#include <unistd.h>

#include "auth.h"
#include "util.h"
#include "zt.h"

static void normalize(void)
{
    char n[9];
    ZT_CHECK(zf_pair_code_normalize("abcd-efgh", n));
    ZT_EQ_STR(n, "ABCDEFGH");
    ZT_CHECK(zf_pair_code_normalize(" ab cd ef gh ", n));
    ZT_CHECK(!zf_pair_code_normalize("ABCD-EFG", n));   /* too short */
    ZT_CHECK(!zf_pair_code_normalize("ABCD-EFGHJ", n)); /* too long */
    ZT_CHECK(!zf_pair_code_normalize("ABCD-EFG0", n));  /* ambiguous char not in alphabet */
    ZT_CHECK(!zf_pair_code_normalize(NULL, n));
}

static void pairing_flow(void)
{
    zf_auth *a = zf_auth_new(NULL, 30);
    char code[16], token[65], csrf[33], csrf2[33];
    int64_t t = 1000000;
    ZT_EQ_INT(zf_auth_new_code(a, t, false, code), ZF_AUTH_OK);
    ZT_EQ_INT(strlen(code), 9);
    ZT_CHECK(code[4] == '-');
    /* immediate second web request is rate limited, CLI (force) is not */
    char code2[16];
    ZT_EQ_INT(zf_auth_new_code(a, t + 10, false, code2), ZF_AUTH_LIMITED);
    ZT_EQ_INT(zf_auth_new_code(a, t + 10, true, code2), ZF_AUTH_OK);
    /* the newer code replaces the older one */
    ZT_EQ_INT(zf_auth_pair(a, code, "x", t + 20, token, csrf), strcmp(code, code2) ? ZF_AUTH_INVALID : ZF_AUTH_OK);
    char lower[16];
    for (int i = 0; code2[i]; i++)
        lower[i] = (char)(code2[i] >= 'A' && code2[i] <= 'Z' ? code2[i] + 32 : code2[i]);
    lower[9] = '\0';
    ZT_EQ_INT(zf_auth_pair(a, lower, "iPhone", t + 30, token, csrf), ZF_AUTH_OK);
    ZT_EQ_INT(strlen(token), 64);
    ZT_EQ_INT(strlen(csrf), 32);
    /* one-time use */
    char token2[65];
    ZT_EQ_INT(zf_auth_pair(a, code2, "again", t + 40, token2, csrf2), ZF_AUTH_INVALID);
    /* sessions */
    ZT_CHECK(zf_auth_check(a, token, csrf2));
    ZT_EQ_STR(csrf2, csrf);
    char bad[65];
    memcpy(bad, token, 65);
    bad[0] = bad[0] == 'a' ? 'b' : 'a';
    ZT_CHECK(!zf_auth_check(a, bad, NULL));
    ZT_CHECK(!zf_auth_check(a, "short", NULL));
    ZT_CHECK(!zf_auth_check(a, NULL, NULL));
    ZT_EQ_INT(zf_auth_logout(a, token), 0);
    ZT_CHECK(!zf_auth_check(a, token, NULL));
    zf_auth_free(a);
}

static void expiry_and_bruteforce(void)
{
    zf_auth *a = zf_auth_new(NULL, 30);
    char code[16], token[65], csrf[33];
    int64_t t = 5000000;
    zf_auth_new_code(a, t, true, code);
    ZT_EQ_INT(zf_auth_pair(a, code, "late", t + ZF_PAIR_CODE_TTL_MS + 1, token, csrf), ZF_AUTH_INVALID);

    /* 5 wrong guesses burn the code even if the 6th is right */
    zf_auth_new_code(a, t + 1000000, true, code);
    for (int i = 0; i < ZF_PAIR_MAX_ATTEMPTS_PER_CODE; i++)
        ZT_EQ_INT(zf_auth_pair(a, "AAAA-AAAA", "guess", t + 1000001 + i, token, csrf), ZF_AUTH_INVALID);
    ZT_EQ_INT(zf_auth_pair(a, code, "real", t + 1000010, token, csrf), ZF_AUTH_INVALID);

    /* global failure limit per minute */
    int64_t t2 = t + 5000000;
    int limited = 0;
    for (int i = 0; i < 20; i++) {
        if (i % 4 == 0)
            zf_auth_new_code(a, t2 + i, true, code);
        if (zf_auth_pair(a, "AAAA-AAAA", "g", t2 + i, token, csrf) == ZF_AUTH_LIMITED)
            limited++;
    }
    ZT_CHECK(limited > 0);
    /* a correct code is also refused while locked out */
    zf_auth_new_code(a, t2 + 100, true, code);
    ZT_EQ_INT(zf_auth_pair(a, code, "x", t2 + 101, token, csrf), ZF_AUTH_LIMITED);
    /* ...and works again after the window */
    ZT_EQ_INT(zf_auth_pair(a, code, "x", t2 + 61001, token, csrf), ZF_AUTH_OK);
    zf_auth_free(a);
}

static void persistence(void)
{
    char dir[] = "/tmp/zfauthXXXXXX";
    ZT_CHECK(mkdtemp(dir) != NULL);
    zf_auth *a = zf_auth_new(dir, 30);
    char code[16], token[65], csrf[33];
    zf_auth_new_code(a, 1, true, code);
    ZT_EQ_INT(zf_auth_pair(a, code, "Desk", 2, token, csrf), ZF_AUTH_OK);
    zf_auth_free(a);

    char path[256];
    snprintf(path, sizeof path, "%s/sessions.json", dir);
    char *text = zf_read_file(path, 65536, NULL);
    ZT_CHECK(text != NULL);
    ZT_CHECK(text && !strstr(text, token)); /* only the hash is stored */
    free(text);

    a = zf_auth_new(dir, 30);
    char c2[33];
    ZT_CHECK(zf_auth_check(a, token, c2));
    ZT_EQ_STR(c2, csrf);
    cJSON *list = zf_auth_list(a);
    ZT_EQ_INT(cJSON_GetArraySize(list), 1);
    const char *id = cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0), "id")->valuestring;
    char idcopy[16];
    zf_strcpy(idcopy, sizeof idcopy, id);
    cJSON_Delete(list);
    ZT_EQ_INT(zf_auth_revoke(a, "nope"), 0);
    ZT_EQ_INT(zf_auth_revoke(a, idcopy), 1);
    ZT_CHECK(!zf_auth_check(a, token, NULL));
    zf_auth_free(a);
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    ZT_RUN(normalize);
    ZT_RUN(pairing_flow);
    ZT_RUN(expiry_and_bruteforce);
    ZT_RUN(persistence);
    ZT_DONE();
}
