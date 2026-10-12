/* conf.c — all settings, in metastreamer.txt on the SD card boot partition.
 *
 * Plain text, key="value", edited on any computer or rewritten by /settings:
 *   country="EE"                  Wi-Fi country code
 *   timezone="Europe/Tallinn"     the goggles' clock is set to this local time (zone files from
 *   timezone_auto="yes"           /usr/share/zoneinfo); yes = follow the browser that opens /settings
 *   network="dhcp"|"static"       with address (a.b.c.d, /24 unless a prefix is given), gateway, dns;
 *   address= gateway= dns=        metastreamer-wifi reads these itself, before the [wifi] section
 *   [wifi]
 *   "name"="password"             one network per line, the first has the highest priority; the password
 *                                 runs to the last quote, so it may contain anything. Values without quotes
 *                                 are taken as they are (settings: without the spaces around them).
 * Nothing ever deletes the file. Passwords never leave the device through the API.
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

#define CONF_TMP "/boot/metastreamer.tmp"
#define ZONEINFO "/usr/share/zoneinfo/"

typedef struct {
    char country[3];
    char tz[64];
    int tz_auto;
    int net_static;
    char addr[20], gw[16], dns[16];
    int n;
    ConfNet nets[CONF_MAX_NET];
} Conf;

static Conf cf = {.tz_auto = 1};
static unsigned gen, sync_gen;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

static int valid_tz(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= sizeof(cf.tz) || s[0] == '/' || strstr(s, "..")) return 0;
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

static int is_country(const char *s) {
    return strlen(s) == 2 && isalpha((unsigned char)s[0]) && isalpha((unsigned char)s[1]);
}

/* "value" -> value (in place) */
static char *unquote(char *v) {
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v[n - 1] = 0; v++; }
    return v;
}

static int is_hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

const char *conf_check_net(const char *ssid, const char *psk) {
    size_t ls = strlen(ssid), lp = strlen(psk);
    if (ls == 0 || ls > 32) return "network name must be 1..32 bytes";
    if (strstr(ssid, "\"=") || ssid[0] == '#' || ssid[0] == '[') return "a name cannot contain '\"=' or start with '#' or '['";
    if (lp && (lp < 8 || lp > 63) && !is_hex64(psk)) return "password must be 8..63 characters";
    return NULL;
}

int conf_read(const char *path) {
    Conf c = {.tz_auto = 1};
    FILE *f = fopen(path, "r");
    if (!f) { cf = c; return -1; }
    char line[512];
    int section = 0;   /* 0 = settings, 1 = [wifi], 2 = unknown */
    for (int ln = 1; fgets(line, sizeof(line), f); ln++) {
        char *s = line;
        if (ln == 1 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;   /* UTF-8 BOM from Windows editors */
        s[strcspn(s, "\r\n")] = 0;
        const char *t = s + strspn(s, " \t");   /* the line after its leading spaces */
        if (!*t || *t == '#') continue;
        if (*t == '[') { section = !strncasecmp(t, "[wifi]", 6) ? 1 : 2; continue; }
        if (section == 1) {   /* "name"="password", or name=password exactly as written */
            char *name = s, *pw;
            if (*t == '"') {
                char *q = strstr(t + 1, "\"=");
                if (!q) { fprintf(stderr, "[conf] %s:%d: expected \"name\"=\"password\", line ignored\n", path, ln); continue; }
                *q = 0; name = (char *)t + 1; pw = q + 2;
            } else {
                char *eq = strchr(s, '=');
                if (!eq) { fprintf(stderr, "[conf] %s:%d: no '=', line ignored\n", path, ln); continue; }
                *eq = 0; pw = eq + 1;
            }
            if (*pw == '"') {   /* a quoted password runs to the last quote */
                char *end = strrchr(pw + 1, '"');
                if (!end) { fprintf(stderr, "[conf] %s:%d: password without its closing quote, line ignored\n", path, ln); continue; }
                *end = 0; pw++;
            }
            const char *e = conf_check_net(name, pw);
            if (e) { fprintf(stderr, "[conf] %s:%d: %s, line ignored\n", path, ln, e); continue; }
            if (c.n == CONF_MAX_NET) { fprintf(stderr, "[conf] more than %d networks, the rest is ignored\n", CONF_MAX_NET); continue; }
            snprintf(c.nets[c.n].ssid, sizeof(c.nets[0].ssid), "%s", name);
            snprintf(c.nets[c.n].psk, sizeof(c.nets[0].psk), "%s", pw);
            c.n++;
        } else if (section == 0) {
            char *eq = strchr(s, '=');
            if (!eq) { fprintf(stderr, "[conf] %s:%d: no '=', line ignored\n", path, ln); continue; }
            *eq = 0;
            char *k = trim(s), *v = unquote(trim(eq + 1));
            if (!strcasecmp(k, "country")) {
                if (is_country(v)) { c.country[0] = (char)toupper((unsigned char)v[0]); c.country[1] = (char)toupper((unsigned char)v[1]); }
                else if (*v) fprintf(stderr, "[conf] %s:%d: country must be a two-letter code\n", path, ln);
            }
            else if (!strcasecmp(k, "timezone_auto")) c.tz_auto = strcasecmp(v, "no") != 0;
            else if (!strcasecmp(k, "timezone")) {
                if (valid_tz(v)) snprintf(c.tz, sizeof(c.tz), "%s", v);
                else if (*v) fprintf(stderr, "[conf] unknown time zone \"%s\", using UTC\n", v);
            }
            else if (!strcasecmp(k, "network")) c.net_static = !strcasecmp(v, "static");
            else if (!strcasecmp(k, "address") && valid_cidr(v)) snprintf(c.addr, sizeof(c.addr), strchr(v, '/') ? "%s" : "%s/24", v);
            else if (!strcasecmp(k, "gateway") && valid_ip(v)) snprintf(c.gw, sizeof(c.gw), "%s", v);
            else if (!strcasecmp(k, "dns") && valid_ip(v)) snprintf(c.dns, sizeof(c.dns), "%s", v);
        }
    }
    fclose(f);
    cf = c;
    return 0;
}

static void apply_tz(void) {
    if (cf.tz[0]) setenv("TZ", cf.tz, 1);
    else unsetenv("TZ");
    tzset();
    gen++;
}

void conf_load(void) {
    if (conf_read(CONF_TXT) < 0) fprintf(stderr, "[conf] no %s, defaults\n", CONF_TXT);
    apply_tz();
    printf("[conf] time zone %s, network %s, %d Wi-Fi network(s)\n",
           cf.tz[0] ? cf.tz : "UTC (not set)", cf.net_static ? cf.addr : "dhcp", cf.n);
}

const char *conf_country(void) { return cf.country; }
const char *conf_timezone(void) { return cf.tz; }
int conf_timezone_auto(void) { return cf.tz_auto; }
unsigned conf_generation(void) { return gen; }
void conf_request_clock_sync(void) { sync_gen++; }
unsigned conf_clock_sync_generation(void) { return sync_gen; }
int conf_net_static(void) { return cf.net_static; }
const char *conf_net_address(void) { return cf.addr; }
const char *conf_net_gateway(void) { return cf.gw; }
const char *conf_net_dns(void) { return cf.dns; }
int conf_wifi_count(void) { return cf.n; }
const ConfNet *conf_wifi(int i) { return i >= 0 && i < cf.n ? &cf.nets[i] : NULL; }

/* write the whole file from c; the boot partition is mounted read-only, so open it just for the write */
static const char *save(const Conf *c) {
    static char txt[8192];
    size_t o = 0;
#define OUT(...) do { if (o < sizeof(txt)) o += snprintf(txt + o, sizeof(txt) - o, __VA_ARGS__); } while (0)
    OUT("# metastreamer settings: key=\"value\". Edit them on any computer,\n"
        "# or at http://<pi>/settings (which rewrites this file).\n\n"
        "# Two-letter country code: Wi-Fi channels and power limits.\n"
        "country=\"%s\"\n\n"
        "# Time zone, an IANA name such as Europe/Tallinn: the goggles' clock is set to this local time.\n"
        "# timezone_auto=\"yes\": follow the browser that opens /settings; \"no\": chosen by hand.\n"
        "timezone=\"%s\"\ntimezone_auto=\"%s\"\n\n"
        "# Address: network=\"dhcp\", or \"static\" with address (a.b.c.d; /24 unless a prefix is given), gateway, dns.\n"
        "# A wrong static address makes the Pi unreachable over Wi-Fi: set network=\"dhcp\" here to undo it.\n"
        "network=\"%s\"\naddress=\"%s\"\ngateway=\"%s\"\ndns=\"%s\"\n\n"
        "# Wi-Fi networks, one per line: \"name\"=\"password\". The first line has the highest priority;\n"
        "# an open network has an empty password (\"\"). The Pi Zero W only sees 2.4 GHz networks.\n"
        "[wifi]\n",
        c->country, c->tz, c->tz_auto ? "yes" : "no",
        c->net_static ? "static" : "dhcp", c->addr, c->gw, c->dns);
    if (!c->n) OUT("# \"My home Wi-Fi\"=\"my password\"\n");
    for (int i = 0; i < c->n; i++) OUT("\"%s\"=\"%s\"\n", c->nets[i].ssid, c->nets[i].psk);
#undef OUT
    if (o >= sizeof(txt)) return "too much data";
    if (system("mount -o remount,rw /boot") != 0) return "cannot write to the SD card (remount failed)";
    int fd = open(CONF_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0 && write(fd, txt, o) == (ssize_t)o && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename(CONF_TMP, CONF_TXT) == 0;
    if (!ok) unlink(CONF_TMP);
    sync();
    if (system("mount -o remount,ro /boot") != 0) fprintf(stderr, "[conf] /boot stays read-write\n");
    return ok ? NULL : "writing metastreamer.txt failed";
}

const char *conf_set_timezone(const char *v, int is_auto) {
    if (!valid_tz(v)) return "unknown time zone";
    if (!strcmp(v, cf.tz) && is_auto == cf.tz_auto) return NULL;
    Conf c = cf;
    snprintf(c.tz, sizeof(c.tz), "%s", v);
    c.tz_auto = is_auto;
    const char *e = save(&c);
    if (e) return e;
    cf = c;
    apply_tz();
    printf("[conf] time zone set to %s (%s)\n", cf.tz, cf.tz_auto ? "auto" : "manual");
    return NULL;
}

const char *conf_set_network(int is_static, const char *addr, const char *gw, const char *dns) {
    Conf c = cf;
    c.net_static = is_static;
    if (is_static) {   /* switching to DHCP keeps the static values for later */
        if (!valid_cidr(addr)) return "address must look like 192.168.1.50";
        if (*gw && !valid_ip(gw)) return "invalid gateway";
        if (*dns && !valid_ip(dns)) return "invalid DNS server";
        snprintf(c.addr, sizeof(c.addr), strchr(addr, '/') ? "%s" : "%s/24", addr);
        snprintf(c.gw, sizeof(c.gw), "%s", gw);
        snprintf(c.dns, sizeof(c.dns), "%s", dns);
    }
    const char *e = save(&c);
    if (e) return e;
    cf = c;
    printf("[conf] network set to %s\n", cf.net_static ? cf.addr : "dhcp");
    if (system("/usr/bin/metastreamer-wifi net </dev/null >/dev/null 2>&1 &") != 0) return "saved, but applying failed";
    return NULL;
}

const char *conf_set_wifi(const ConfNet *nets, int n) {
    if (n < 0 || n > CONF_MAX_NET) return "too many networks";
    for (int i = 0; i < n; i++) if (conf_check_net(nets[i].ssid, nets[i].psk)) return "invalid network";
    Conf c = cf;
    c.n = n;
    memcpy(c.nets, nets, sizeof(ConfNet) * n);
    const char *e = save(&c);
    if (e) return e;
    cf = c;
    printf("[conf] %d Wi-Fi network(s) saved from /settings\n", cf.n);
    return NULL;
}
