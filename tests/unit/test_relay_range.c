#include "relay.h"
#include "zt.h"

static void ranges(void)
{
    int64_t s, e;
    ZT_EQ_INT(zf_range_parse("bytes=0-99", &s, &e), 0);
    ZT_EQ_INT(s, 0);
    ZT_EQ_INT(e, 99);
    ZT_EQ_INT(zf_range_parse("bytes=500-", &s, &e), 0);
    ZT_EQ_INT(s, 500);
    ZT_EQ_INT(e, -1);
    ZT_EQ_INT(zf_range_parse("bytes=-300", &s, &e), 0);
    ZT_EQ_INT(s, -1);
    ZT_EQ_INT(e, 300);
    ZT_EQ_INT(zf_range_parse("BYTES=1-2", &s, &e), 0);
    const char *bad[] = {"bytes=5-1", "bytes=0-1,5-6", "bytes=",
                         "bytes=-",   "bytes=-0",      "items=0-1",
                         "bytes=a-b", "bytes=1-2x",    "bytes=99999999999999999999-",
                         NULL};
    for (int i = 0; bad[i]; i++)
        ZT_EQ_INT(zf_range_parse(bad[i], &s, &e), -1);
    ZT_EQ_INT(zf_range_parse(NULL, &s, &e), -1);
}

static void content_ranges(void)
{
    int64_t a, b, t;
    ZT_EQ_INT(zf_content_range_parse("bytes 0-99/1000", &a, &b, &t), 0);
    ZT_EQ_INT(a, 0);
    ZT_EQ_INT(b, 99);
    ZT_EQ_INT(t, 1000);
    ZT_EQ_INT(zf_content_range_parse("bytes 10-19/*", &a, &b, &t), 0);
    ZT_EQ_INT(t, -1);
    ZT_EQ_INT(zf_content_range_parse("bytes */1000", &a, &b, &t), 0);
    ZT_EQ_INT(a, -1);
    ZT_EQ_INT(t, 1000);
    ZT_EQ_INT(zf_content_range_parse("bytes 9-1/10", &a, &b, &t), -1);
    ZT_EQ_INT(zf_content_range_parse("bytes 0-1", &a, &b, &t), -1);
    ZT_EQ_INT(zf_content_range_parse("items 0-1/2", &a, &b, &t), -1);
}

int main(void)
{
    ZT_RUN(ranges);
    ZT_RUN(content_ranges);
    ZT_DONE();
}
