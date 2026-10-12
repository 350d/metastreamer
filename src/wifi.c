/* wifi.c — Wi-Fi: the networks from the [wifi] section of metastreamer.txt (kept by conf.c),
 * the wpa_supplicant config made from them at boot ("metastreamer --wifi-conf"), and the /api/wifi and
 * /api/net pages. /settings saves through conf.c and runs "metastreamer-wifi reload".
 * Passwords never leave the device through the API.
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

#define WIFI_TOOL "/usr/bin/metastreamer-wifi"

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

int wifi_gen_conf(const char *txt, const char *conf) {
    if (conf_read(txt) < 0) fprintf(stderr, "[wifi] no %s: no networks\n", txt);
    int n = conf_wifi_count();
    const char *country = conf_country();
    static char out[8192];
    size_t o = 0;
#define OUT(...) do { if (o < sizeof(out)) o += snprintf(out + o, sizeof(out) - o, __VA_ARGS__); } while (0)
    /* no ctrl_interface: this wpa_supplicant is built without it and rejects the whole file; reload = SIGHUP */
    OUT("# generated from %s by metastreamer --wifi-conf, edit that file instead\n", txt);
    if (country[0]) OUT("country=%s\n", country);
    for (int i = 0; i < n; i++) {
        const ConfNet *w = conf_wifi(i);
        OUT("network={\n\tssid=");   /* hex: any bytes in the name are safe */
        for (const unsigned char *p = (const unsigned char *)w->ssid; *p; p++) OUT("%02x", *p);
        OUT("\n\tscan_ssid=1\n\tpriority=%d\n", n - i);
        if (!w->psk[0]) OUT("\tkey_mgmt=NONE\n");
        else if (is_hex64(w->psk)) OUT("\tpsk=%s\n", w->psk);
        else OUT("\tpsk=\"%s\"\n", w->psk);   /* wpa_supplicant takes everything up to the last quote */
        OUT("}\n");
    }
#undef OUT
    if (o >= sizeof(out)) { fprintf(stderr, "[wifi] config too large\n"); return 1; }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", conf);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0 && write(fd, out, o) == (ssize_t)o;
    if (fd >= 0) close(fd);
    if (!ok || rename(tmp, conf) != 0) { perror(conf); unlink(tmp); return 1; }
    printf("[wifi] %s: %d network(s)%s%s\n", conf, n, country[0] ? ", country " : "", country);
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
    jp(&j, "{\"available\":%s,\"file\":%s,\"wlan\":%s,\"country\":",
       access(WIFI_TOOL, X_OK) == 0 ? "true" : "false", access(CONF_TXT, F_OK) == 0 ? "true" : "false",
       access("/sys/class/net/wlan0", F_OK) == 0 ? "true" : "false");
    js(&j, conf_country());
    jp(&j, ",\"networks\":[");
    for (int i = 0; i < conf_wifi_count(); i++) {
        jp(&j, "%s{\"ssid\":", i ? "," : "");
        js(&j, conf_wifi(i)->ssid);
        jp(&j, ",\"saved_password\":%s}", conf_wifi(i)->psk[0] ? "true" : "false");
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

/* form: per network n (name), p (password), o (saved network whose password to keep when p is empty) */
const char *wifi_save(const char *body) {
    static char err[160];
    static ConfNet nw[CONF_MAX_NET];
    if (access(WIFI_TOOL, X_OK) != 0) return "Wi-Fi control is not available on this system";
    int n = 0, cur = -1;
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
        if (!strcmp(k, "n")) {
            if (!*v) return "network name is empty";
            for (int i = 0; i < n; i++)
                if (!strcmp(nw[i].ssid, v)) { snprintf(err, sizeof(err), "\"%s\" is listed twice", v); return err; }
            if (strlen(v) > 32) { snprintf(err, sizeof(err), "\"%.40s\": name longer than 32 bytes", v); return err; }
            if (n == CONF_MAX_NET) return "too many networks";
            cur = n++;
            memset(&nw[cur], 0, sizeof(nw[cur]));
            snprintf(nw[cur].ssid, sizeof(nw[0].ssid), "%s", v);
        } else if (!strcmp(k, "p") && cur >= 0) {
            if (strlen(v) > 64) return "password too long";
            snprintf(nw[cur].psk, sizeof(nw[0].psk), "%s", v);
        } else if (!strcmp(k, "o") && cur >= 0 && !nw[cur].psk[0]) {
            for (int i = 0; i < conf_wifi_count(); i++)
                if (!strcmp(conf_wifi(i)->ssid, v)) { memcpy(nw[cur].psk, conf_wifi(i)->psk, sizeof(nw[0].psk)); break; }
        }
    }
    for (int i = 0; i < n; i++) {
        const char *e = conf_check_net(nw[i].ssid, nw[i].psk);
        if (e) { snprintf(err, sizeof(err), "\"%s\": %s", nw[i].ssid, e); return err; }
    }
    const char *e = conf_set_wifi(nw, n);
    if (e) return e;
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
