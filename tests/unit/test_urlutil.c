#include "urlutil.h"
#include "zt.h"

static void parse(void)
{
    zf_url u;
    ZT_EQ_INT(zf_url_parse("http://Jellyfin.LAN:8096/web/index.html?x=1", &u), 0);
    ZT_EQ_STR(u.scheme, "http");
    ZT_EQ_STR(u.host, "jellyfin.lan");
    ZT_EQ_INT(u.port, 8096);
    ZT_EQ_STR(u.path, "/web/index.html?x=1");
    ZT_EQ_INT(zf_url_parse("https://[fd00::1]/jf", &u), 0);
    ZT_EQ_STR(u.host, "[fd00::1]");
    ZT_EQ_INT(u.port, 443);
    ZT_EQ_INT(zf_url_parse("https://h?q", &u), 0);
    ZT_EQ_STR(u.path, "/?q");
    const char *bad[] = {"ftp://h/",    "http://user:pw@h/", "http://h:0/",    "http://h:99999/",
                         "http:///x",   "http://h/a b",      "http://h/#frag", "file:///etc/passwd",
                         "http://h\n/", "javascript:x",      "http://h:12a/"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        ZT_EQ_INT(zf_url_parse(bad[i], &u), -1);
    zf_url a, b;
    zf_url_parse("http://h:8096/a", &a);
    zf_url_parse("http://H:8096/b?c", &b);
    ZT_CHECK(zf_url_same_origin(&a, &b));
    zf_url_parse("https://h:8096/a", &b);
    ZT_CHECK(!zf_url_same_origin(&a, &b));
}

static void normalize(void)
{
    char out[256];
    ZT_EQ_INT(zf_normalize_server_url("  http://192.168.1.20:8096/ ", out, sizeof out), 0);
    ZT_EQ_STR(out, "http://192.168.1.20:8096");
    ZT_EQ_INT(zf_normalize_server_url("https://media.example.org:443/jellyfin//", out, sizeof out), 0);
    ZT_EQ_STR(out, "https://media.example.org/jellyfin");
    ZT_EQ_INT(zf_normalize_server_url("http://h/?api_key=x", out, sizeof out), -1);
    ZT_EQ_INT(zf_normalize_server_url("http://u:p@h", out, sizeof out), -1);
    ZT_EQ_INT(zf_normalize_server_url("h:8096", out, sizeof out), -1);
}

static void location(void)
{
    char out[256];
    ZT_EQ_INT(zf_url_resolve_location("http://h:8096/a?b", "/Videos/1/stream", out, sizeof out), 0);
    ZT_EQ_STR(out, "http://h:8096/Videos/1/stream");
    ZT_EQ_INT(zf_url_resolve_location("http://h/a", "https://other/x", out, sizeof out), 0);
    ZT_EQ_STR(out, "https://other/x");
    ZT_EQ_INT(zf_url_resolve_location("http://h/a", "//evil/x", out, sizeof out), -1);
    ZT_EQ_INT(zf_url_resolve_location("http://h/a", "relative", out, sizeof out), -1);
    ZT_EQ_INT(zf_url_resolve_location("http://h/a", "/x\r\nInjected: 1", out, sizeof out), -1);
}

static void encode_and_ids(void)
{
    char out[64];
    ZT_EQ_INT(zf_urlencode("a b&c=d/é", out, sizeof out), 0);
    ZT_EQ_STR(out, "a%20b%26c%3Dd%2F%C3%A9");
    ZT_EQ_INT(zf_urlencode("abcdef", out, 4), -1);
    char id[33];
    ZT_CHECK(zf_jf_id_normalize("A1000000000000000000000000000001", id, sizeof id));
    ZT_EQ_STR(id, "a1000000000000000000000000000001");
    ZT_CHECK(zf_jf_id_normalize("a1000000-0000-0000-0000-000000000001", id, sizeof id));
    ZT_EQ_STR(id, "a1000000000000000000000000000001");
    ZT_CHECK(!zf_jf_id_normalize("a10000000000000000000000000000g1", id, sizeof id));
    ZT_CHECK(!zf_jf_id_normalize("../../etc/passwd", id, sizeof id));
    ZT_CHECK(!zf_jf_id_normalize("a1000000-0000-0000-0000_000000000001", id, sizeof id));
    ZT_CHECK(!zf_jf_id_normalize("", id, sizeof id));
}

int main(void)
{
    ZT_RUN(parse);
    ZT_RUN(normalize);
    ZT_RUN(location);
    ZT_RUN(encode_and_ids);
    ZT_DONE();
}
