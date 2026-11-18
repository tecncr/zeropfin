#include <stdlib.h>

#include "compat.h"
#include "util.h"
#include "zt.h"

static cJSON *lib;
static zf_config cfg;

static const cJSON *item_by_name(const char *name)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(lib, "items"))
    {
        if (!strcmp(cJSON_GetObjectItem(it, "Name")->valuestring, name))
            return it;
    }
    fprintf(stderr, "fixture item %s not found\n", name);
    exit(1);
}

static zf_compat eval_item(const char *name, int *chosen)
{
    zf_compat c;
    *chosen = zf_compat_choose(cJSON_GetObjectItem(item_by_name(name), "MediaSources"), &cfg, &c);
    return c;
}

static bool reason_has(const zf_compat *c, const char *needle)
{
    for (int i = 0; i < c->n_reasons; i++)
        if (strstr(c->reasons[i], needle))
            return true;
    return false;
}

static void playable(void)
{
    int idx;
    zf_compat c = eval_item("Harbor Lights", &idx);
    ZT_EQ_INT(idx, 0);
    ZT_CHECK(c.playable);
    ZT_EQ_INT(c.n_reasons, 0);
    ZT_EQ_STR(c.container, "mkv");
    ZT_EQ_INT(c.width, 1920);
    ZT_EQ_INT(c.level, 41);
    ZT_EQ_INT(c.audio_index, 1);
    ZT_EQ_STR(c.audio_codec, "aac");
    ZT_EQ_INT(c.subtitle_index, 2); /* first embedded text subtitle is auto-shown by ZeroPlay */
    ZT_CHECK(c.n_warnings >= 1);

    c = eval_item("Northern Line", &idx);
    ZT_CHECK(c.playable); /* multi-name container list "mov,mp4,..." */
    ZT_EQ_STR(c.container, "mov");
}

static void rejected(void)
{
    int idx;
    zf_compat c = eval_item("Quiet Engine", &idx);
    ZT_EQ_INT(idx, -1);
    ZT_CHECK(!c.playable);
    ZT_CHECK(reason_has(&c, "HEVC"));

    c = eval_item("Paper Moons", &idx);
    ZT_CHECK(!c.playable);
    ZT_CHECK(reason_has(&c, "10-bit") || reason_has(&c, "High 10"));

    c = eval_item("Copper Field", &idx);
    ZT_CHECK(!c.playable);
    ZT_CHECK(reason_has(&c, "first audio track is DTS"));

    c = eval_item("Fast Water", &idx);
    ZT_CHECK(!c.playable);
    ZT_CHECK(reason_has(&c, "Frame rate 60"));
    ZT_CHECK(reason_has(&c, "level 4.2"));

    c = eval_item("Heavy Rain", &idx);
    ZT_CHECK(!c.playable);
    ZT_CHECK(reason_has(&c, "25.0 Mbps"));

    c = eval_item("Glass Garden", &idx);
    ZT_CHECK(!c.playable);
    ZT_CHECK(reason_has(&c, "AVI container"));
    ZT_CHECK(reason_has(&c, "MPEG4"));
}

static void second_source(void)
{
    int idx;
    zf_compat c = eval_item("Two Sources", &idx);
    ZT_EQ_INT(idx, 1);
    ZT_CHECK(c.playable);
    ZT_EQ_INT(c.width, 1280);
    ZT_EQ_STR(c.media_source_id, "f0000000000000000000000000000051");
}

static void synthetic(void)
{
    /* No media sources at all, remote protocol, cover-art "video" streams. */
    zf_compat c;
    ZT_EQ_INT(zf_compat_choose(NULL, &cfg, &c), -1);
    ZT_CHECK(!c.playable);
    cJSON *ms = cJSON_Parse("{\"Id\":\"x\",\"Protocol\":\"Http\",\"Container\":\"mp4\",\"MediaStreams\":["
                            "{\"Index\":0,\"Type\":\"Video\",\"Codec\":\"mjpeg\"},"
                            "{\"Index\":1,\"Type\":\"Video\",\"Codec\":\"h264\",\"Profile\":\"Main\",\"Level\":30,"
                            "\"Width\":640,\"Height\":480,\"RealFrameRate\":25,\"BitDepth\":8}]}");
    zf_compat_evaluate(ms, &cfg, &c);
    ZT_CHECK(!c.playable);
    ZT_CHECK(reason_has(&c, "Http sources"));
    ZT_EQ_STR(c.video_codec, "h264"); /* mjpeg cover art skipped */
    cJSON_Delete(ms);
    /* tighter config: 720p limit */
    zf_config small = cfg;
    small.max_height = 720;
    small.max_width = 1280;
    int idx = zf_compat_choose(cJSON_GetObjectItem(item_by_name("Harbor Lights"), "MediaSources"), &small, &c);
    ZT_EQ_INT(idx, -1);
    ZT_CHECK(reason_has(&c, "Resolution 1920x1080"));
    cJSON *j = zf_compat_to_json(&c);
    ZT_CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "playable")));
    ZT_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(j, "reasons")) >= 1);
    cJSON_Delete(j);
}

int main(void)
{
    char *text = zf_read_file("fixtures/library.json", 4 << 20, NULL);
    if (!text) {
        fprintf(stderr, "cannot read fixtures/library.json (run from tests/)\n");
        return 1;
    }
    lib = cJSON_Parse(text);
    free(text);
    zf_config_defaults(&cfg);
    ZT_RUN(playable);
    ZT_RUN(rejected);
    ZT_RUN(second_source);
    ZT_RUN(synthetic);
    cJSON_Delete(lib);
    ZT_DONE();
}
