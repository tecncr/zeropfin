#include "http.h"
#include "zt.h"

static void paths(void)
{
    const char *ok[] = {"/index.html", "/assets/index-3f9a2b.js", "/assets/inter-latin-wght.woff2",
                        "/licenses/Inter-OFL.txt", "/favicon.svg"};
    const char *bad[] = {"/../etc/passwd", "/assets/../../x", "/.env",      "/assets/.hidden",
                         "//etc/passwd",   "/assets//x.js",   "/a%2e%2e/b", "/a b",
                         "/a\\b",          "relative",        "",           "/x?y",
                         "/assets/./x.js"};
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
        ZT_CHECK(zf_static_path_ok(ok[i]));
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (zf_static_path_ok(bad[i]))
            fprintf(stderr, "accepted bad path: %s\n", bad[i]);
        ZT_CHECK(!zf_static_path_ok(bad[i]));
    }
    ZT_CHECK(!zf_static_path_ok(NULL));
}

static void mime(void)
{
    ZT_EQ_STR(zf_mime_for("/index.html"), "text/html; charset=utf-8");
    ZT_EQ_STR(zf_mime_for("/assets/a.js"), "text/javascript; charset=utf-8");
    ZT_EQ_STR(zf_mime_for("/assets/a.css"), "text/css; charset=utf-8");
    ZT_EQ_STR(zf_mime_for("/assets/inter.woff2"), "font/woff2");
    ZT_EQ_STR(zf_mime_for("/icon.svg"), "image/svg+xml");
    ZT_EQ_STR(zf_mime_for("/manifest.webmanifest"), "application/manifest+json");
    ZT_EQ_STR(zf_mime_for("/noext"), "application/octet-stream");
    ZT_EQ_STR(zf_mime_for("/x.exe"), "application/octet-stream");
}

int main(void)
{
    ZT_RUN(paths);
    ZT_RUN(mime);
    ZT_DONE();
}
