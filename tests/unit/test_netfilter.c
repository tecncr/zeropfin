#include <arpa/inet.h>
#include <netinet/in.h>

#include "netfilter.h"
#include "util.h"
#include "zt.h"

static struct sockaddr_storage addr(const char *s)
{
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
    if (inet_pton(AF_INET, s, &s4->sin_addr) == 1)
        s4->sin_family = AF_INET;
    else if (inet_pton(AF_INET6, s, &s6->sin6_addr) == 1)
        s6->sin6_family = AF_INET6;
    return ss;
}

static void private_ranges(void)
{
    const char *yes[] = {"127.0.0.1",    "10.1.2.3",     "172.16.0.1",        "172.31.255.254",
                         "192.168.1.50", "169.254.10.1", "100.64.0.1",        "::1",
                         "fd12:3456::1", "fe80::1",      "::ffff:192.168.1.9"};
    const char *no[] = {"8.8.8.8",     "172.32.0.1",     "172.15.255.255", "100.128.0.1",
                        "2001:db8::1", "::ffff:1.1.1.1", "192.169.0.1"};
    for (size_t i = 0; i < sizeof yes / sizeof *yes; i++) {
        struct sockaddr_storage a = addr(yes[i]);
        if (!zf_addr_is_private((struct sockaddr *)&a))
            fprintf(stderr, "expected private: %s\n", yes[i]);
        ZT_CHECK(zf_addr_is_private((struct sockaddr *)&a));
    }
    for (size_t i = 0; i < sizeof no / sizeof *no; i++) {
        struct sockaddr_storage a = addr(no[i]);
        if (zf_addr_is_private((struct sockaddr *)&a))
            fprintf(stderr, "expected public: %s\n", no[i]);
        ZT_CHECK(!zf_addr_is_private((struct sockaddr *)&a));
    }
}

static void cidr(void)
{
    struct sockaddr_storage a = addr("203.0.113.77");
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&a, "203.0.113.0/24"), 1);
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&a, "203.0.112.0/24"), 0);
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&a, "203.0.113.77"), 1);
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&a, "203.0.113.0/33"), -1);
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&a, "nonsense"), -1);
    struct sockaddr_storage m = addr("::ffff:203.0.113.5");
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&m, "203.0.113.0/28"), 1);
    struct sockaddr_storage v6 = addr("2001:db8:1::5");
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&v6, "2001:db8::/32"), 1);
    ZT_EQ_INT(zf_cidr_match((struct sockaddr *)&v6, "2001:db9::/32"), 0);
}

static void hosts(void)
{
    zf_config c;
    zf_config_defaults(&c);
    zf_strcpy(c.allowed_hosts[0], sizeof c.allowed_hosts[0], "tv.lan");
    c.n_allowed_hosts = 1;
    ZT_CHECK(zf_host_allowed("192.168.1.30:8080", "raspberrypi", &c));
    ZT_CHECK(zf_host_allowed("[fe80::1]:8080", "raspberrypi", &c));
    ZT_CHECK(zf_host_allowed("localhost:8080", "raspberrypi", &c));
    ZT_CHECK(zf_host_allowed("RaspberryPi.local:8080", "raspberrypi", &c));
    ZT_CHECK(zf_host_allowed("raspberrypi", "raspberrypi", &c));
    ZT_CHECK(zf_host_allowed("TV.lan", "raspberrypi", &c));
    ZT_CHECK(!zf_host_allowed("evil.example.com", "raspberrypi", &c)); /* DNS rebinding */
    ZT_CHECK(!zf_host_allowed("raspberrypi.evil.com", "raspberrypi", &c));
    ZT_CHECK(!zf_host_allowed(NULL, "raspberrypi", &c));
    ZT_CHECK(!zf_host_allowed("", "raspberrypi", &c));
    ZT_CHECK(!zf_host_allowed("192.168.1.30:abc", "raspberrypi", &c));
    ZT_CHECK(!zf_host_allowed("[zz::1]", "raspberrypi", &c));
}

static void origins(void)
{
    ZT_CHECK(zf_origin_allowed(NULL, "192.168.1.30:8080"));
    ZT_CHECK(zf_origin_allowed("http://192.168.1.30:8080", "192.168.1.30:8080"));
    ZT_CHECK(zf_origin_allowed("https://tv.lan", "tv.lan"));
    ZT_CHECK(!zf_origin_allowed("http://evil.com", "192.168.1.30:8080"));
    ZT_CHECK(!zf_origin_allowed("null", "192.168.1.30:8080"));
    ZT_CHECK(!zf_origin_allowed("http://192.168.1.30:8081", "192.168.1.30:8080"));
}

int main(void)
{
    ZT_RUN(private_ranges);
    ZT_RUN(cidr);
    ZT_RUN(hosts);
    ZT_RUN(origins);
    ZT_DONE();
}
