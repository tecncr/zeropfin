#include <stdlib.h>

#include "config.h"
#include "util.h"
#include "zt.h"

static void defaults(void)
{
    zf_config c;
    zf_config_defaults(&c);
    ZT_EQ_INT(c.listen_port, 8080);
    ZT_EQ_STR(c.listen_address, "::");
    ZT_EQ_INT(c.max_level, 41);
    ZT_EQ_INT(c.max_bitrate, 12000000);
    ZT_CHECK(zf_config_list_has(&c.audio_codecs[0][0], c.n_audio_codecs, "AAC"));
    ZT_CHECK(!zf_config_list_has(&c.audio_codecs[0][0], c.n_audio_codecs, "dts"));
    ZT_CHECK(c.pairing_show_on_display);
}

static void parse_ok(void)
{
    zf_config c;
    zf_config_defaults(&c);
    char err[160] = "";
    const char *text = "# comment\n"
                       "listen_port = 9090   # trailing\n"
                       "zeroplay_args = --audio-device plughw:CARD=Headphones,DEV=0 --no-downmixing\n"
                       "allowed_hosts = tv.lan, Receiver.Local\n"
                       "audio_codecs = aac,ac3\n"
                       "max_bitrate = 8000000\n"
                       "pairing_show_on_display = no\n";
    ZT_EQ_INT(zf_config_parse(&c, text, err, sizeof err), 0);
    ZT_EQ_INT(c.listen_port, 9090);
    ZT_EQ_INT(c.n_zeroplay_args, 3);
    ZT_EQ_STR(c.zeroplay_args[1], "plughw:CARD=Headphones,DEV=0");
    ZT_EQ_INT(c.n_allowed_hosts, 2);
    ZT_EQ_STR(c.allowed_hosts[1], "receiver.local");
    ZT_EQ_INT(c.n_audio_codecs, 2);
    ZT_EQ_INT(c.max_bitrate, 8000000);
    ZT_CHECK(!c.pairing_show_on_display);
}

static void parse_errors(void)
{
    const char *bad[] = {
        "unknown_key = 1\n",
        "listen_port = 0\n",
        "listen_port = 70000\n",
        "listen_port\n",
        "zeroplay_args = --ws-url ws://evil\n", /* not allowlisted */
        "zeroplay_args = --audio-device\n",     /* missing value */
        "zeroplay_args = --vol --no-audio\n",   /* flag as value */
        "web_root = relative/path\n",
        "state_dir = /var/lib/../etc\n",
        "verbose = maybe\n",
        "device_name = evil\"name\n",
        "audio_codecs = aac;rm -rf\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        zf_config c;
        zf_config_defaults(&c);
        char err[160] = "";
        int rc = zf_config_parse(&c, bad[i], err, sizeof err);
        ZT_EQ_INT(rc, -1);
        ZT_CHECK(err[0] != '\0');
    }
}

static void shipped_sample(void)
{
    /* packaging/zeropfin.conf must stay valid for the installer. */
    char *text = zf_read_file("../packaging/zeropfin.conf", 65536, NULL);
    ZT_CHECK(text != NULL);
    zf_config c;
    zf_config_defaults(&c);
    char err[160] = "";
    ZT_EQ_INT(zf_config_parse(&c, text ? text : "", err, sizeof err), 0);
    if (err[0])
        fprintf(stderr, "sample config: %s\n", err);
    ZT_EQ_INT(c.listen_port, 8080);
    ZT_EQ_STR(c.zeroplay_path, "/usr/lib/zeropfin/zeroplay");
    free(text);
}

int main(void)
{
    ZT_RUN(defaults);
    ZT_RUN(parse_ok);
    ZT_RUN(parse_errors);
    ZT_RUN(shipped_sample);
    ZT_DONE();
}
