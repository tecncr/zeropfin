#include <stdlib.h>

#include "jellyfin.h"
#include "reporter.h"
#include "zt.h"

static void header(void)
{
    char h[512];
    ZT_EQ_INT(zf_jf_auth_header("Living Room", "zeropfin-abc", "0123token", h, sizeof h), 0);
    ZT_CHECK(!strncmp(h,
                      "Authorization: MediaBrowser Client=\"Zeropfin\", Device=\"Living Room\", "
                      "DeviceId=\"zeropfin-abc\", Version=\"",
                      89));
    ZT_CHECK(strstr(h, "Token=\"0123token\"") != NULL);
    ZT_CHECK(!strstr(h, "X-Emby"));
    ZT_EQ_INT(zf_jf_auth_header("d", "id", NULL, h, sizeof h), 0);
    ZT_CHECK(!strstr(h, "Token="));
}

static void versions(void)
{
    ZT_EQ_INT(zf_jf_version_number("10.10.7"), 101007);
    ZT_EQ_INT(zf_jf_version_number("12.4.0"), 120400);
    ZT_EQ_INT(zf_jf_version_number("10.9"), 100900);
    ZT_CHECK(zf_jf_version_number("10.9.11") < ZF_JF_MIN_VERSION);
    ZT_CHECK(zf_jf_version_number("10.10.0") >= ZF_JF_MIN_VERSION);
    ZT_EQ_INT(zf_jf_version_number("garbage"), 0);
    ZT_EQ_INT(zf_jf_version_number(NULL), 0);
}

static void summary(void)
{
    char *text = zf_read_file("fixtures/library.json", 4 << 20, NULL);
    ZT_CHECK(text != NULL);
    cJSON *lib = cJSON_Parse(text);
    free(text);
    const cJSON *it, *episode = NULL, *movie = NULL;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(lib, "items"))
    {
        const char *n = cJSON_GetObjectItem(it, "Name")->valuestring;
        if (!strcmp(n, "Low Tide"))
            episode = it;
        if (!strcmp(n, "Northern Line"))
            movie = it;
    }
    cJSON *m = zf_jf_item_summary(movie);
    ZT_EQ_STR(cJSON_GetObjectItem(m, "type")->valuestring, "Movie");
    ZT_EQ_INT(cJSON_GetObjectItem(m, "runtimeMs")->valuedouble, 4800000);
    ZT_EQ_INT(cJSON_GetObjectItem(m, "resumeMs")->valuedouble, 600000);
    ZT_EQ_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(m, "poster"), "type")->valuestring, "Primary");
    ZT_CHECK(cJSON_GetObjectItem(m, "backdrop") != NULL);
    ZT_CHECK(cJSON_GetObjectItem(m, "MediaSources") == NULL); /* never forwarded */
    ZT_CHECK(cJSON_GetObjectItem(m, "Path") == NULL);
    cJSON_Delete(m);

    cJSON *e = zf_jf_item_summary(episode);
    ZT_EQ_STR(cJSON_GetObjectItem(e, "seriesName")->valuestring, "Lantern Bay");
    ZT_EQ_INT(cJSON_GetObjectItem(e, "index")->valuedouble, 2);
    ZT_EQ_INT(cJSON_GetObjectItem(e, "parentIndex")->valuedouble, 1);
    /* episode poster uses the series poster; thumb uses the episode still */
    ZT_EQ_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(e, "poster"), "tag")->valuestring, "ser0001");
    ZT_EQ_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(e, "thumb"), "tag")->valuestring, "e0066");
    cJSON_Delete(e);

    cJSON *bad = cJSON_Parse("{\"Id\":\"../../x\",\"Name\":\"x\"}");
    ZT_CHECK(zf_jf_item_summary(bad) == NULL);
    cJSON_Delete(bad);
    cJSON_Delete(lib);
}

static void report_bodies(void)
{
    zf_report r;
    memset(&r, 0, sizeof r);
    r.kind = ZF_REPORT_PROGRESS;
    strcpy(r.item_id, "a0000000000000000000000000000001");
    strcpy(r.media_source_id, "f0000000000000000000000000000001");
    strcpy(r.play_session_id, "abc123");
    r.audio_stream_index = 1;
    r.subtitle_stream_index = -1;
    r.paused = true;
    r.event_name = "Pause";
    r.has_position = true;
    r.position_ticks = 6000000000LL;
    cJSON *b = zf_report_body(&r);
    ZT_EQ_STR(cJSON_GetObjectItem(b, "PlayMethod")->valuestring, "DirectPlay");
    ZT_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(b, "IsPaused")));
    ZT_EQ_INT((long long)cJSON_GetObjectItem(b, "PositionTicks")->valuedouble, 6000000000LL);
    ZT_EQ_STR(cJSON_GetObjectItem(b, "EventName")->valuestring, "Pause");
    ZT_CHECK(cJSON_GetObjectItem(b, "SubtitleStreamIndex") == NULL);
    cJSON_Delete(b);
    r.kind = ZF_REPORT_STOP;
    r.has_position = false;
    r.failed = true;
    b = zf_report_body(&r);
    ZT_CHECK(cJSON_GetObjectItem(b, "PositionTicks") == NULL); /* untrusted position omitted */
    ZT_CHECK(cJSON_IsTrue(cJSON_GetObjectItem(b, "Failed")));
    ZT_CHECK(cJSON_GetObjectItem(b, "PlayMethod") == NULL);
    cJSON_Delete(b);
    ZT_EQ_STR(zf_report_path(ZF_REPORT_START), "/Sessions/Playing");
    ZT_EQ_STR(zf_report_path(ZF_REPORT_STOP), "/Sessions/Playing/Stopped");
}

int main(void)
{
    ZT_RUN(header);
    ZT_RUN(versions);
    ZT_RUN(summary);
    ZT_RUN(report_bodies);
    ZT_DONE();
}
