/* wifi.h — Wi-Fi networks kept in wifi.txt on the SD card boot partition. */
#ifndef MS_WIFI_H
#define MS_WIFI_H
#include <stddef.h>

/* metastreamer --wifi-conf TXT CONF: wpa_supplicant config from wifi.txt (missing TXT = no networks). 0 = ok */
int wifi_gen_conf(const char *txt, const char *conf);
/* JSON for GET /api/wifi: network names (never passwords) and the current connection */
size_t wifi_json(char *out, size_t cap);
/* POST /api/wifi form body: country, then per network n (name), p (password), o (saved network whose
 * password to keep when p is empty). Validates, rewrites wifi.txt, applies. NULL = ok, else the error. */
const char *wifi_save(const char *body);
/* JSON for GET /api/net: saved mode and static values (conf.c) plus what wlan0 has right now */
size_t wifi_net_json(char *out, size_t cap);
#endif
