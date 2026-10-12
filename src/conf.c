/* conf.c — bridge settings kept in metastreamer.txt on the SD card boot partition.
 *
 * "timezone = Europe/Tallinn": the goggles' clock is set to local time in this zone (zone files from
 * /usr/share/zoneinfo, so summer time is handled). "timezone_auto = yes": /settings keeps it equal to
 * the time zone of the browser that opens it; "no": chosen by hand.
 * "network = dhcp | static" with "address = 192.168.1.50/24", "gateway", "dns": the Wi-Fi address.
 * metastreamer-wifi reads the network keys itself; a static address skips waiting for DHCP at boot.
 */
#include "conf.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define CONF_TXT "/boot/metastreamer.txt"
#define CONF_TMP "/boot/metastreamer.tmp"
#define ZONEINFO "/usr/share/zoneinfo/"

static char tz[64];
static int tz_auto = 1;
static unsigned gen, sync_gen;
static int net_static;
static char net_addr[20], net_gw[16], net_dns[16];

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

static int valid_tz(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= sizeof(tz) || s[0] == '/' || strstr(s, "..")) return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("/_+-", *p)) return 0;
    char path[128];
    snprintf(path, sizeof(path), ZONEINFO "%s", s);
    return access(path, R_OK) == 0;
}

static int valid_ip(const char *s) {
    struct in_addr a;
    return *s && inet_pton(AF_INET, s, &a) == 1;
}

/* "a.b.c.d/nn" with nn 1..32, or "a.b.c.d" (then /24) */
static int valid_cidr(const char *s) {
    char ip[20];
    const char *sl = strchr(s, '/');
    if (!sl) return valid_ip(s);
    if ((size_t)(sl - s) >= sizeof(ip)) return 0;
    memcpy(ip, s, sl - s); ip[sl - s] = 0;
    char *end;
    long p = strtol(sl + 1, &end, 10);
    return valid_ip(ip) && sl[1] && !*end && p >= 1 && p <= 32;
}

static void apply(void) {
    if (tz[0]) setenv("TZ", tz, 1);
    else unsetenv("TZ");
    tzset();
    gen++;
}

void conf_load(void) {
    FILE *f = fopen(CONF_TXT, "r");
    if (f) {
        char line[256];
        for (int ln = 1; fgets(line, sizeof(line), f); ln++) {
            char *s = line;
            if (ln == 1 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;
            s = trim(s);
            if (!*s || *s == '#') continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            if (!strcasecmp(k, "timezone_auto")) tz_auto = strcasecmp(v, "no") != 0;
            else if (!strcasecmp(k, "timezone")) {
                if (valid_tz(v)) snprintf(tz, sizeof(tz), "%s", v);
                else if (*v) fprintf(stderr, "[conf] unknown time zone \"%s\", using UTC\n", v);
            }
            else if (!strcasecmp(k, "network")) net_static = !strcasecmp(v, "static");
            else if (!strcasecmp(k, "address") && valid_cidr(v)) snprintf(net_addr, sizeof(net_addr), strchr(v, '/') ? "%s" : "%s/24", v);
            else if (!strcasecmp(k, "gateway") && valid_ip(v)) snprintf(net_gw, sizeof(net_gw), "%s", v);
            else if (!strcasecmp(k, "dns") && valid_ip(v)) snprintf(net_dns, sizeof(net_dns), "%s", v);
        }
        fclose(f);
    }
    apply();
    printf("[conf] time zone %s, network %s\n", tz[0] ? tz : "UTC (not set)", net_static ? net_addr : "dhcp");
}

const char *conf_timezone(void) { return tz; }
int conf_timezone_auto(void) { return tz_auto; }
unsigned conf_generation(void) { return gen; }
void conf_request_clock_sync(void) { sync_gen++; }
unsigned conf_clock_sync_generation(void) { return sync_gen; }
int conf_net_static(void) { return net_static; }
const char *conf_net_address(void) { return net_addr; }
const char *conf_net_gateway(void) { return net_gw; }
const char *conf_net_dns(void) { return net_dns; }

/* write all settings; the boot partition is mounted read-only, so open it just for the write */
static const char *save(const char *z, int z_auto, int st, const char *addr, const char *gw, const char *dns) {
    char txt[1024];
    int n = snprintf(txt, sizeof(txt),
        "# metastreamer settings.\n"
        "# timezone: IANA name such as Europe/Tallinn; the goggles' clock is set to this local time.\n"
        "# timezone_auto: yes = follow the browser that opens /settings, no = chosen by hand.\n"
        "timezone = %s\ntimezone_auto = %s\n\n"
        "# network: dhcp, or static with address (a.b.c.d, or a.b.c.d/prefix; default /24), gateway and dns.\n"
        "# A wrong static address makes the bridge unreachable: set network = dhcp here to undo it.\n"
        "network = %s\naddress = %s\ngateway = %s\ndns = %s\n",
        z, z_auto ? "yes" : "no", st ? "static" : "dhcp", addr, gw, dns);
    if (system("mount -o remount,rw /boot") != 0) return "cannot write to the SD card (remount failed)";
    int fd = open(CONF_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int ok = fd >= 0 && write(fd, txt, n) == n && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename(CONF_TMP, CONF_TXT) == 0;
    if (!ok) unlink(CONF_TMP);
    sync();
    if (system("mount -o remount,ro /boot") != 0) fprintf(stderr, "[conf] /boot stays read-write\n");
    return ok ? NULL : "writing metastreamer.txt failed";
}

const char *conf_set_timezone(const char *v, int is_auto) {
    if (!valid_tz(v)) return "unknown time zone";
    if (!strcmp(v, tz) && is_auto == tz_auto) return NULL;
    const char *e = save(v, is_auto, net_static, net_addr, net_gw, net_dns);
    if (e) return e;
    snprintf(tz, sizeof(tz), "%s", v);
    tz_auto = is_auto;
    apply();
    printf("[conf] time zone set to %s (%s)\n", tz, tz_auto ? "auto" : "manual");
    return NULL;
}

const char *conf_set_network(int is_static, const char *addr, const char *gw, const char *dns) {
    if (is_static) {
        if (!valid_cidr(addr)) return "address must look like 192.168.1.50";
        if (*gw && !valid_ip(gw)) return "invalid gateway";
        if (*dns && !valid_ip(dns)) return "invalid DNS server";
    } else {   /* keep the static values for later, only the mode changes */
        addr = net_addr; gw = net_gw; dns = net_dns;
    }
    const char *e = save(tz, tz_auto, is_static, addr, gw, dns);
    if (e) return e;
    net_static = is_static;
    snprintf(net_addr, sizeof(net_addr), "%s", addr);
    snprintf(net_gw, sizeof(net_gw), "%s", gw);
    snprintf(net_dns, sizeof(net_dns), "%s", dns);
    printf("[conf] network set to %s\n", net_static ? net_addr : "dhcp");
    if (system("/usr/bin/metastreamer-wifi net </dev/null >/dev/null 2>&1 &") != 0) return "saved, but applying failed";
    return NULL;
}
