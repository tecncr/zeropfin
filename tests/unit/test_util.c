#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"
#include "util.h"
#include "zt.h"

static void ticks(void)
{
    int64_t t = 0, ms = 0;
    ZT_EQ_INT(zf_ms_to_ticks(0, &t), 0);
    ZT_EQ_INT(t, 0);
    ZT_EQ_INT(zf_ms_to_ticks(1, &t), 0);
    ZT_EQ_INT(t, 10000);
    ZT_EQ_INT(zf_ms_to_ticks(5400000, &t), 0); /* 1h30 */
    ZT_EQ_INT(t, 54000000000LL);
    ZT_EQ_INT(zf_ms_to_ticks(-1, &t), -1);
    ZT_EQ_INT(zf_ms_to_ticks(INT64_MAX / 10000, &t), 0);
    ZT_EQ_INT(zf_ms_to_ticks(INT64_MAX / 10000 + 1, &t), -1); /* overflow */
    ZT_EQ_INT(zf_ticks_to_ms(54000000000LL, &ms), 0);
    ZT_EQ_INT(ms, 5400000);
    ZT_EQ_INT(zf_ticks_to_ms(9999, &ms), 0); /* sub-millisecond truncates */
    ZT_EQ_INT(ms, 0);
    ZT_EQ_INT(zf_ticks_to_ms(-5, &ms), -1);
    ZT_EQ_INT(zf_ticks_to_ms(INT64_MAX, &ms), 0);
    ZT_EQ_INT(ms, INT64_MAX / 10000);
}

static void parse_i64(void)
{
    int64_t v;
    ZT_EQ_INT(zf_parse_i64("42", 0, 100, &v), 0);
    ZT_EQ_INT(v, 42);
    ZT_EQ_INT(zf_parse_i64("-7", -10, 10, &v), 0);
    ZT_EQ_INT(v, -7);
    ZT_EQ_INT(zf_parse_i64("101", 0, 100, &v), -1);
    ZT_EQ_INT(zf_parse_i64("", 0, 100, &v), -1);
    ZT_EQ_INT(zf_parse_i64("12a", 0, 100, &v), -1);
    ZT_EQ_INT(zf_parse_i64("+1", 0, 100, &v), -1);
    ZT_EQ_INT(zf_parse_i64(" 1", 0, 100, &v), -1);
    ZT_EQ_INT(zf_parse_i64("9223372036854775807", 0, INT64_MAX, &v), 0);
    ZT_EQ_INT(zf_parse_i64("9223372036854775808", 0, INT64_MAX, &v), -1);
    ZT_EQ_INT(zf_parse_i64("-9223372036854775808", INT64_MIN, 0, &v), 0);
    ZT_CHECK(v == INT64_MIN);
}

static void redact(void)
{
    char out[256];
    zf_redact_url("http://jf.lan:8096/Videos/x/stream?static=true&api_key=SECRET", out, sizeof out);
    ZT_EQ_STR(out, "http://jf.lan:8096/Videos/x/stream?[redacted]");
    zf_redact_url("https://user:pw@host/path", out, sizeof out);
    ZT_EQ_STR(out, "https://[redacted]@host/path");
    ZT_CHECK(!strstr(out, "pw"));
    zf_redact_url("http://host/a#frag", out, sizeof out);
    ZT_EQ_STR(out, "http://host/a?[redacted]");
    zf_redact_url("http://host/plain", out, 8); /* truncation stays terminated */
    ZT_EQ_INT(strlen(out), 7);
}

static void strings(void)
{
    char b[4];
    ZT_EQ_INT(zf_strcpy(b, sizeof b, "abc"), 0);
    ZT_EQ_STR(b, "abc");
    ZT_EQ_INT(zf_strcpy(b, sizeof b, "abcd"), -1);
    ZT_EQ_STR(b, "abc");
    ZT_EQ_INT(zf_fmt(b, sizeof b, "%d", 1234), -1);
    ZT_CHECK(zf_has_ctl("a\nb"));
    ZT_CHECK(zf_has_ctl("a\x7f"));
    ZT_CHECK(!zf_has_ctl("plain text"));
    ZT_CHECK(zf_ct_equal("abc", "abc", 3));
    ZT_CHECK(!zf_ct_equal("abc", "abd", 3));
    zf_buf buf;
    zf_buf_init(&buf, 8);
    ZT_EQ_INT(zf_buf_append(&buf, "12345", 5), 0);
    ZT_EQ_INT(zf_buf_append(&buf, "678", 3), 0);
    ZT_EQ_INT(zf_buf_append(&buf, "9", 1), -1);
    ZT_CHECK(buf.overflow);
    ZT_EQ_STR(buf.data, "12345678");
    zf_buf_free(&buf);
    char hex[33];
    ZT_EQ_INT(zf_random_hex(hex, sizeof hex, 16), 0);
    ZT_EQ_INT(strlen(hex), 32);
    char hex2[33];
    zf_random_hex(hex2, sizeof hex2, 16);
    ZT_CHECK(strcmp(hex, hex2) != 0);
}

static void sha256_vectors(void)
{
    char h[65];
    zf_sha256_hex("", 0, h);
    ZT_EQ_STR(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    zf_sha256_hex("abc", 3, h);
    ZT_EQ_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    zf_sha256_hex(m, strlen(m), h);
    ZT_EQ_STR(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* one million 'a' via incremental updates */
    zf_sha256 c;
    zf_sha256_init(&c);
    char block[1000];
    memset(block, 'a', sizeof block);
    for (int i = 0; i < 1000; i++)
        zf_sha256_update(&c, block, sizeof block);
    uint8_t d[32];
    zf_sha256_final(&c, d);
    zf_hex_encode(d, 32, h);
    ZT_EQ_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void files(void)
{
    char dir[] = "/tmp/zftestXXXXXX";
    ZT_CHECK(mkdtemp(dir) != NULL);
    char path[256];
    snprintf(path, sizeof path, "%s/f.json", dir);
    ZT_EQ_INT(zf_write_file_atomic(path, "hello", 5, 0600), 0);
    struct stat st;
    ZT_EQ_INT(stat(path, &st), 0);
    ZT_EQ_INT(st.st_mode & 0777, 0600);
    size_t len = 0;
    char *t = zf_read_file(path, 100, &len);
    ZT_CHECK(t != NULL);
    ZT_EQ_INT(len, 5);
    ZT_EQ_STR(t, "hello");
    free(t);
    ZT_CHECK(zf_read_file(path, 3, &len) == NULL); /* over limit */
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    ZT_RUN(ticks);
    ZT_RUN(parse_i64);
    ZT_RUN(redact);
    ZT_RUN(strings);
    ZT_RUN(sha256_vectors);
    ZT_RUN(files);
    ZT_DONE();
}
