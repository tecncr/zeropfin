#include "compat.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static bool jbool(const cJSON *o, const char *k)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

__attribute__((format(printf, 2, 3))) static void add_reason(zf_compat *c, const char *fmt, ...)
{
    c->playable = false;
    if (c->n_reasons >= ZF_COMPAT_MAX_MSG)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->reasons[c->n_reasons++], sizeof c->reasons[0], fmt, ap);
    va_end(ap);
}

__attribute__((format(printf, 2, 3))) static void add_warning(zf_compat *c, const char *fmt, ...)
{
    if (c->n_warnings >= ZF_COMPAT_MAX_MSG)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->warnings[c->n_warnings++], sizeof c->warnings[0], fmt, ap);
    va_end(ap);
}

static void upper_name(const char *codec, char *out, size_t cap)
{
    size_t i = 0;
    for (; codec[i] && i + 1 < cap; i++)
        out[i] = (char)toupper((unsigned char)codec[i]);
    out[i] = '\0';
}

/* Container lists come as "mov,mp4,m4a,3gp,3g2,mj2" or "mkv"/"matroska". */
static bool container_ok(const char *container, const zf_config *cfg)
{
    char tmp[128];
    if (zf_strcpy(tmp, sizeof tmp, container) < 0)
        return false;
    char *save = NULL;
    for (char *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!strcasecmp(t, "matroska"))
            t = (char *)(uintptr_t)"mkv";
        if (zf_config_list_has(&cfg->containers[0][0], cfg->n_containers, t))
            return true;
    }
    return false;
}

static bool profile_ok(const char *profile)
{
    static const char *ok[] = {"Baseline", "Constrained Baseline", "Main", "Extended", "High"};
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
        if (!strcasecmp(profile, ok[i]))
            return true;
    return false;
}

static void stream_label(const cJSON *s, char *out, size_t cap)
{
    const char *t = jstr(s, "DisplayTitle");
    if (!t)
        t = jstr(s, "Title");
    if (!t)
        t = jstr(s, "Codec");
    zf_strcpy(out, cap, t ? t : "Unknown");
}

void zf_compat_evaluate(const cJSON *ms, const zf_config *cfg, zf_compat *c)
{
    memset(c, 0, sizeof *c);
    c->playable = true;
    c->audio_index = -1;
    c->subtitle_index = -1;
    if (!cJSON_IsObject(ms)) {
        add_reason(c, "No media source information is available.");
        return;
    }
    const char *id = jstr(ms, "Id");
    if (id)
        zf_strcpy(c->media_source_id, sizeof c->media_source_id, id);

    const char *protocol = jstr(ms, "Protocol");
    if (protocol && strcasecmp(protocol, "File") != 0)
        add_reason(c, "%s sources (remote or live streams) are not supported.", protocol);
    if (jbool(ms, "IsRemote"))
        add_reason(c, "Remote media sources are not supported.");
    if (jbool(ms, "IsInfiniteStream") || jbool(ms, "RequiresOpening"))
        add_reason(c, "Live streams are not supported.");
    const char *vtype = jstr(ms, "VideoType");
    if (vtype && strcasecmp(vtype, "VideoFile") != 0)
        add_reason(c, "%s images (DVD/Blu-ray/ISO) cannot be played directly.", vtype);

    const char *container = jstr(ms, "Container");
    if (container) {
        zf_strcpy(c->container, sizeof c->container, container);
        char *comma = strchr(c->container, ',');
        if (comma)
            *comma = '\0';
    }
    if (!container || !container_ok(container, cfg)) {
        char up[32];
        upper_name(container ? c->container : "unknown", up, sizeof up);
        add_reason(c, "The %s container is not supported (MP4, MOV or MKV required).", up);
    }

    c->bitrate = (int64_t)jnum(ms, "Bitrate", 0);
    if (c->bitrate > cfg->max_bitrate) {
        char a[48], b[48];
        snprintf(a, sizeof a, "%lld.%lld", (long long)(c->bitrate / 1000000), (long long)(c->bitrate / 100000 % 10));
        snprintf(b, sizeof b, "%lld", (long long)(cfg->max_bitrate / 1000000));
        add_reason(c, "Bitrate %s Mbps exceeds the %s Mbps limit for this receiver.", a, b);
    }

    const cJSON *streams = cJSON_GetObjectItemCaseSensitive(ms, "MediaStreams");
    const cJSON *video = NULL, *first_video = NULL, *audio = NULL, *sub = NULL;
    int n_audio = 0;
    int best_v = 1 << 30, best_fv = 1 << 30, best_a = 1 << 30, best_s = 1 << 30;
    const cJSON *s;
    cJSON_ArrayForEach(s, streams)
    {
        if (jbool(s, "IsExternal"))
            continue;
        const char *type = jstr(s, "Type");
        const char *codec = jstr(s, "Codec");
        int idx = (int)jnum(s, "Index", 1 << 29);
        if (!type)
            continue;
        if (!strcmp(type, "Video")) {
            /* Embedded cover art shows up as mjpeg/png "video" streams. */
            if (codec && (!strcasecmp(codec, "mjpeg") || !strcasecmp(codec, "png")))
                continue;
            if (idx < best_fv) {
                best_fv = idx;
                first_video = s;
            }
            if (codec && !strcasecmp(codec, "h264") && idx < best_v) {
                best_v = idx;
                video = s;
            }
        } else if (!strcmp(type, "Audio")) {
            n_audio++;
            if (idx < best_a) {
                best_a = idx;
                audio = s;
            }
        } else if (!strcmp(type, "Subtitle")) {
            if (idx < best_s) {
                best_s = idx;
                sub = s;
            }
        }
    }

    if (!video) {
        const char *codec = first_video ? jstr(first_video, "Codec") : NULL;
        char up[24];
        upper_name(codec ? codec : "unknown", up, sizeof up);
        if (first_video)
            add_reason(c, "Video codec %s is not supported; ZeroPlay decodes H.264 only.", up);
        else
            add_reason(c, "No video stream was found.");
    } else {
        zf_strcpy(c->video_codec, sizeof c->video_codec, "h264");
        const char *profile = jstr(video, "Profile");
        if (profile)
            zf_strcpy(c->video_profile, sizeof c->video_profile, profile);
        c->width = (int)jnum(video, "Width", 0);
        c->height = (int)jnum(video, "Height", 0);
        c->level = (int)jnum(video, "Level", 0);
        c->bit_depth = (int)jnum(video, "BitDepth", 0);
        c->fps = jnum(video, "RealFrameRate", 0);
        if (c->fps <= 0)
            c->fps = jnum(video, "AverageFrameRate", 0);
        const char *pixfmt = jstr(video, "PixelFormat");

        if (profile && !profile_ok(profile))
            add_reason(c, "H.264 %s profile is not supported (Baseline, Main or High 8-bit only).", profile);
        if (c->bit_depth > 8 || (pixfmt && (strstr(pixfmt, "10") || strstr(pixfmt, "12"))))
            add_reason(c, "10-bit H.264 (Hi10P) cannot be hardware decoded.");
        else if (pixfmt && (strstr(pixfmt, "422") || strstr(pixfmt, "444")))
            add_reason(c, "H.264 with %s chroma is not supported (4:2:0 only).", pixfmt);
        if (c->level > cfg->max_level) {
            char a[16], b[16];
            snprintf(a, sizeof a, "%d.%d", c->level / 10, c->level % 10);
            snprintf(b, sizeof b, "%d.%d", cfg->max_level / 10, cfg->max_level % 10);
            add_reason(c, "H.264 level %s exceeds the supported level %s.", a, b);
        } else if (c->level <= 0) {
            add_warning(c, "H.264 level is unknown; playback is not guaranteed.");
        }
        if (c->width > cfg->max_width || c->height > cfg->max_height) {
            char a[32], b[32];
            snprintf(a, sizeof a, "%dx%d", c->width, c->height);
            snprintf(b, sizeof b, "%dx%d", cfg->max_width, cfg->max_height);
            add_reason(c, "Resolution %s exceeds the %s limit.", a, b);
        }
        if (c->fps > (double)cfg->max_fps + 0.5) {
            char a[16], b[16];
            snprintf(a, sizeof a, "%d", (int)(c->fps + 0.5));
            snprintf(b, sizeof b, "%d", cfg->max_fps);
            add_reason(c, "Frame rate %s fps exceeds the %s fps limit.", a, b);
        }
        if (jbool(video, "IsInterlaced"))
            add_warning(c, "Interlaced video has not been validated on this receiver.");
    }

    if (audio) {
        c->audio_index = (int)jnum(audio, "Index", -1);
        const char *codec = jstr(audio, "Codec");
        zf_strcpy(c->audio_codec, sizeof c->audio_codec, codec ? codec : "unknown");
        c->audio_channels = (int)jnum(audio, "Channels", 0);
        stream_label(audio, c->audio_label, sizeof c->audio_label);
        if (!codec || !zf_config_list_has(&cfg->audio_codecs[0][0], cfg->n_audio_codecs, codec)) {
            char up[24];
            upper_name(c->audio_codec, up, sizeof up);
            add_reason(c,
                       "The first audio track is %s, which is not supported (ZeroPlay always plays the first "
                       "track; AAC, MP3, AC3, FLAC or Opus required).",
                       up);
        } else if (n_audio > 1) {
            add_warning(c, "Only the first audio track (%s) can be played.", c->audio_label);
        }
    } else {
        add_warning(c, "This media has no audio track.");
    }

    if (sub) {
        c->subtitle_index = (int)jnum(sub, "Index", -1);
        stream_label(sub, c->subtitle_label, sizeof c->subtitle_label);
        if (jbool(sub, "IsTextSubtitleStream"))
            add_warning(c, "Embedded subtitles (%s) will be shown automatically.", c->subtitle_label);
        else
            c->subtitle_index = -1; /* bitmap subtitles are not rendered */
    }
}

int zf_compat_choose(const cJSON *sources, const zf_config *cfg, zf_compat *out)
{
    const cJSON *s;
    int i = 0;
    bool have_first = false;
    zf_compat first;
    cJSON_ArrayForEach(s, sources)
    {
        zf_compat c;
        zf_compat_evaluate(s, cfg, &c);
        if (c.playable) {
            *out = c;
            return i;
        }
        if (!have_first) {
            first = c;
            have_first = true;
        }
        i++;
    }
    if (have_first)
        *out = first;
    else
        zf_compat_evaluate(NULL, cfg, out);
    return -1;
}

cJSON *zf_compat_to_json(const zf_compat *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "playable", c->playable);
    cJSON_AddStringToObject(o, "method", c->playable ? "DirectPlay" : "None");
    cJSON *r = cJSON_AddArrayToObject(o, "reasons");
    for (int i = 0; i < c->n_reasons; i++)
        cJSON_AddItemToArray(r, cJSON_CreateString(c->reasons[i]));
    cJSON *w = cJSON_AddArrayToObject(o, "warnings");
    for (int i = 0; i < c->n_warnings; i++)
        cJSON_AddItemToArray(w, cJSON_CreateString(c->warnings[i]));
    cJSON *m = cJSON_AddObjectToObject(o, "media");
    cJSON_AddStringToObject(m, "container", c->container);
    cJSON_AddStringToObject(m, "videoCodec", c->video_codec);
    cJSON_AddStringToObject(m, "videoProfile", c->video_profile);
    cJSON_AddNumberToObject(m, "width", c->width);
    cJSON_AddNumberToObject(m, "height", c->height);
    cJSON_AddNumberToObject(m, "level", c->level);
    cJSON_AddNumberToObject(m, "fps", (double)(int)(c->fps * 100 + 0.5) / 100.0);
    cJSON_AddNumberToObject(m, "bitrate", (double)c->bitrate);
    cJSON_AddStringToObject(m, "audioCodec", c->audio_codec);
    cJSON_AddNumberToObject(m, "audioChannels", c->audio_channels);
    cJSON_AddStringToObject(m, "audio", c->audio_label);
    cJSON_AddStringToObject(m, "subtitles", c->subtitle_label);
    return o;
}
