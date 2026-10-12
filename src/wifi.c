/* wifi.c — Wi-Fi networks kept in wifi.txt on the SD card boot partition.
 *
 * wifi.txt is plain text: "country = XX" and one network per line, "name = password".
 * It is the only place networks are stored and nothing ever deletes it. At boot metastreamer-wifi turns it
 * into /run/wpa_supplicant.conf through "metastreamer --wifi-conf"; /settings rewrites it and runs
 * "metastreamer-wifi reload". Passwords never leave the device through the API.
 */
#include "wifi.h"
#include "conf.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define WIFI_TXT  "/boot/wifi.txt"
#define WIFI_TMP  "/boot/wifi.tmp"
#define WIFI_TOOL "/usr/bin/metastreamer-wifi"
#define MAX_NET   16

typedef struct { char ssid[33], psk[65]; } Net;
typedef struct { char country[3]; int n; Net net[MAX_NET]; } Cfg;

static const char HEADER[] =
    "# Wi-Fi networks for the metastreamer. One network per line:   name = password\n"
    "# The first line has the highest priority. An open network has nothing after \"=\".\n"
    "# Spaces around the name and the password are ignored; a name cannot contain \"=\".\n"
    "# Whole-line comments start with #. The Pi Zero W only sees 2.4 GHz networks.\n"
    "# country: two-letter code of your country (correct channels and power limits).\n"
    "# This file is also edited from http://<bridge>/settings, which rewrites it in this format.\n\n";

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

static int is_hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

static int is_country(const char *s) {
    return strlen(s) == 2 && isalpha((unsigned char)s[0]) && isalpha((unsigned char)s[1]);
}

/* NULL = ok, else what is wrong */
static const char *check_net(const char *ssid, const char *psk) {
    size_t ls = strlen(ssid), lp = strlen(psk);
    if (ls == 0 || ls > 32) return "network name must be 1..32 bytes";
    if (lp && (lp < 8 || lp > 63) && !is_hex64(psk)) return "password must be 8..63 characters";
    return NULL;
}

static int load(const char *path, Cfg *c) {
    memset(c, 0, sizeof(*c));
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    for (int ln = 1; fgets(line, sizeof(line), f); ln++) {
        char *s = line;
        if (ln == 1 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;   /* UTF-8 BOM from Windows editors */
        s = trim(s);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "[wifi] %s:%d: no '=', line ignored\n", path, ln); continue; }
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!strcasecmp(k, "country")) {
            if (is_country(v)) { c->country[0] = (char)toupper((unsigned char)v[0]); c->country[1] = (char)toupper((unsigned char)v[1]); }
            else if (*v) fprintf(stderr, "[wifi] %s:%d: country must be a two-letter code\n", path, ln);
            continue;
        }
        const char *e = check_net(k, v);
        if (e) { fprintf(stderr, "[wifi] %s:%d: %s, line ignored\n", path, ln, e); continue; }
        if (c->n == MAX_NET) { fprintf(stderr, "[wifi] more than %d networks, the rest is ignored\n", MAX_NET); break; }
        snprintf(c->net[c->n].ssid, sizeof(c->net[0].ssid), "%s", k);
        snprintf(c->net[c->n].psk, sizeof(c->net[0].psk), "%s", v);
        c->n++;
    }
    fclose(f);
    return 0;
}

static int write_file(const char *tmp, const char *path, const char *data, size_t n) {
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, data, n) == (ssize_t)n && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int wifi_gen_conf(const char *txt, const char *conf) {
    Cfg c;
    if (load(txt, &c) < 0) fprintf(stderr, "[wifi] no %s: no networks\n", txt);
    static char out[8192];
    size_t o = 0;
#define OUT(...) do { if (o < sizeof(out)) o += snprintf(out + o, sizeof(out) - o, __VA_ARGS__); } while (0)
    /* no ctrl_interface: this wpa_supplicant is built without it and rejects the whole file; reload = SIGHUP */
    OUT("# generated from %s by metastreamer --wifi-conf, edit that file instead\n", txt);
    if (c.country[0]) OUT("country=%s\n", c.country);
    for (int i = 0; i < c.n; i++) {
        OUT("network={\n\tssid=");   /* hex: any bytes in the name are safe */
        for (const unsigned char *p = (const unsigned char *)c.net[i].ssid; *p; p++) OUT("%02x", *p);
        OUT("\n\tscan_ssid=1\n\tpriority=%d\n", c.n - i);
        if (!c.net[i].psk[0]) OUT("\tkey_mgmt=NONE\n");
        else if (is_hex64(c.net[i].psk)) OUT("\tpsk=%s\n", c.net[i].psk);
        else OUT("\tpsk=\"%s\"\n", c.net[i].psk);   /* wpa_supplicant takes everything up to the last quote */
        OUT("}\n");
    }
#undef OUT
    if (o >= sizeof(out)) { fprintf(stderr, "[wifi] config too large\n"); return 1; }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", conf);
    if (write_file(tmp, conf, out, o) != 0) { perror(conf); return 1; }
    printf("[wifi] %s: %d network(s)%s%s\n", conf, c.n, c.country[0] ? ", country " : "", c.country);
    return 0;
}

/* ---------------- JSON ---------------- */
typedef struct { char *p; size_t cap, n; } J;

static void jp(J *j, const char *fmt, ...) {
    if (j->n + 1 >= j->cap) return;
    va_list a;
    va_start(a, fmt);
    int r = vsnprintf(j->p + j->n, j->cap - j->n, fmt, a);
    va_end(a);
    if (r > 0) j->n += (size_t)r;
    if (j->n >= j->cap) j->n = j->cap - 1;
}

static void js(J *j, const char *s) {
    jp(j, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') jp(j, "\\%c", *p);
        else if (*p < 0x20) jp(j, "\\u%04x", *p);
        else jp(j, "%c", *p);
    }
    jp(j, "\"");
}

/* "iw dev wlan0 link": SSID and signal of the current connection */
static void link_info(char *ssid, size_t sl, int *signal) {
    ssid[0] = 0; *signal = 0;
    FILE *p = popen("iw dev wlan0 link 2>/dev/null", "r");
    if (!p) return;
    char line[256];
    while (fgets(line, sizeof(line), p)) {
        char *s = trim(line);
        if (!strncmp(s, "SSID: ", 6)) snprintf(ssid, sl, "%s", s + 6);
        else if (!strncmp(s, "signal: ", 8)) *signal = atoi(s + 8);
    }
    pclose(p);
}

static void wlan_ip(char *ip, size_t n) {
    ip[0] = 0;
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa) != 0) return;
    for (i = ifa; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "wlan0")) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, n);
            break;
        }
    freeifaddrs(ifa);
}

size_t wifi_json(char *out, size_t cap) {
    J j = {out, cap, 0};
    out[0] = 0;
    Cfg c;
    int have = load(WIFI_TXT, &c) == 0;
    jp(&j, "{\"available\":%s,\"file\":%s,\"wlan\":%s,\"country\":",
       access(WIFI_TOOL, X_OK) == 0 ? "true" : "false", have ? "true" : "false",
       access("/sys/class/net/wlan0", F_OK) == 0 ? "true" : "false");
    js(&j, c.country);
    jp(&j, ",\"networks\":[");
    for (int i = 0; i < c.n; i++) {
        jp(&j, "%s{\"ssid\":", i ? "," : "");
        js(&j, c.net[i].ssid);
        jp(&j, ",\"saved_password\":%s}", c.net[i].psk[0] ? "true" : "false");
    }
    char ssid[160], ip[INET_ADDRSTRLEN];
    int sig;
    link_info(ssid, sizeof(ssid), &sig);
    wlan_ip(ip, sizeof(ip));
    jp(&j, "],\"link\":");
    if (!ssid[0]) jp(&j, "null");
    else { jp(&j, "{\"ssid\":"); js(&j, ssid); jp(&j, ",\"signal_dbm\":%d,\"ip\":", sig); js(&j, ip); jp(&j, "}"); }
    jp(&j, "}");
    return j.n;
}

/* ---------------- saving from /settings ---------------- */
static void form_decode(char *s) {
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '+') *o = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            unsigned v; sscanf(s + 1, "%2x", &v); *o = (char)v; s += 2;
        } else *o = *s;
    }
    *o = 0;
}

static int has_ctrl(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return 1;
    return 0;
}

const char *wifi_save(const char *body) {
    static char err[160];
    if (access(WIFI_TOOL, X_OK) != 0) return "Wi-Fi control is not available on this system";
    Cfg old, nw;
    load(WIFI_TXT, &old);
    memset(&nw, 0, sizeof(nw));
    memcpy(nw.country, old.country, sizeof(nw.country));   /* kept unless the form sends one */
    int cur = -1;
    for (const char *p = body; *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        char kv[400];
        if (len >= sizeof(kv)) return "field too long";
        memcpy(kv, p, len); kv[len] = 0;
        p += len + (amp ? 1 : 0);
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = kv, *v = eq + 1;
        form_decode(k); form_decode(v);
        if (has_ctrl(v)) return "control characters are not allowed";
        if (!strcmp(k, "country")) {
            v = trim(v);
            if (*v && !is_country(v)) return "country must be a two-letter code";
            nw.country[0] = (char)toupper((unsigned char)v[0]);
            nw.country[1] = *v ? (char)toupper((unsigned char)v[1]) : 0;
        } else if (!strcmp(k, "n")) {
            v = trim(v);
            if (!*v) return "network name is empty";
            if (strlen(v) > 32) { snprintf(err, sizeof(err), "\"%.40s\": name longer than 32 bytes", v); return err; }
            if (strchr(v, '=') || *v == '#') { snprintf(err, sizeof(err), "\"%s\": a name cannot contain \"=\" or start with \"#\"", v); return err; }
            for (int i = 0; i < nw.n; i++)
                if (!strcmp(nw.net[i].ssid, v)) { snprintf(err, sizeof(err), "\"%s\" is listed twice", v); return err; }
            if (nw.n == MAX_NET) return "too many networks";
            cur = nw.n++;
            snprintf(nw.net[cur].ssid, sizeof(nw.net[0].ssid), "%s", v);
        } else if (!strcmp(k, "p") && cur >= 0) {
            size_t lv = strlen(v);
            if (lv && (v[0] == ' ' || v[lv - 1] == ' ')) {
                snprintf(err, sizeof(err), "\"%s\": a password cannot start or end with a space", nw.net[cur].ssid);
                return err;
            }
            if (lv > 64) return "password too long";
            snprintf(nw.net[cur].psk, sizeof(nw.net[0].psk), "%s", v);
        } else if (!strcmp(k, "o") && cur >= 0 && !nw.net[cur].psk[0]) {
            for (int i = 0; i < old.n; i++)
                if (!strcmp(old.net[i].ssid, v)) { memcpy(nw.net[cur].psk, old.net[i].psk, sizeof(nw.net[0].psk)); break; }
        }
    }
    for (int i = 0; i < nw.n; i++) {
        const char *e = check_net(nw.net[i].ssid, nw.net[i].psk);
        if (e) { snprintf(err, sizeof(err), "\"%s\": %s", nw.net[i].ssid, e); return err; }
    }

    static char txt[8192];
    size_t o = (size_t)snprintf(txt, sizeof(txt), "%scountry = %s\n\n", HEADER, nw.country);
    for (int i = 0; i < nw.n && o < sizeof(txt); i++)
        o += (size_t)snprintf(txt + o, sizeof(txt) - o, "%s = %s\n", nw.net[i].ssid, nw.net[i].psk);
    if (o >= sizeof(txt)) return "too much data";

    /* the boot partition is mounted read-only: open it just for the write */
    if (system("mount -o remount,rw /boot") != 0) return "cannot write to the SD card (remount failed)";
    int ok = write_file(WIFI_TMP, WIFI_TXT, txt, o) == 0;
    sync();
    if (system("mount -o remount,ro /boot") != 0) fprintf(stderr, "[wifi] /boot stays read-write\n");
    if (!ok) return "writing wifi.txt failed";
    printf("[wifi] wifi.txt saved from /settings: %d network(s)\n", nw.n);
    if (system(WIFI_TOOL " reload </dev/null >/dev/null 2>&1 &") != 0) return "saved, but applying failed";
    return NULL;
}

/* ---------------- address (/api/net) ---------------- */
size_t wifi_net_json(char *out, size_t cap) {
    char addr[24] = "", gw[16] = "", dns[16] = "";
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa) == 0) {
        for (i = ifa; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_netmask && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "wlan0")) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
                unsigned m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr), p = 0;
                while (m & 0x80000000u) { p++; m <<= 1; }
                snprintf(addr, sizeof(addr), "%s/%u", ip, p);
                break;
            }
        freeifaddrs(ifa);
    }
    FILE *f = fopen("/proc/net/route", "r");   /* default route: destination 0 */
    if (f) {
        char line[256], ifn[16];
        unsigned dst, g;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%15s %x %x", ifn, &dst, &g) == 3 && !strcmp(ifn, "wlan0") && dst == 0) {
                struct in_addr a = {g};
                inet_ntop(AF_INET, &a, gw, sizeof(gw));
                break;
            }
        fclose(f);
    }
    f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) if (sscanf(line, "nameserver %15s", dns) == 1) break;
        fclose(f);
    }
    J j = {out, cap, 0};
    out[0] = 0;
    jp(&j, "{\"mode\":\"%s\",\"address\":", conf_net_static() ? "static" : "dhcp");
    js(&j, conf_net_address()); jp(&j, ",\"gateway\":"); js(&j, conf_net_gateway());
    jp(&j, ",\"dns\":"); js(&j, conf_net_dns());
    jp(&j, ",\"current\":{\"address\":"); js(&j, addr); jp(&j, ",\"gateway\":"); js(&j, gw);
    jp(&j, ",\"dns\":"); js(&j, dns); jp(&j, "}}");
    return j.n;
}
