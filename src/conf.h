/* conf.h — bridge settings kept in metastreamer.txt on the SD card boot partition. */
#ifndef MS_CONF_H
#define MS_CONF_H

/* read metastreamer.txt and apply it (TZ for local time) */
void conf_load(void);
/* IANA time zone such as "Europe/Tallinn", "" = UTC */
const char *conf_timezone(void);
/* 1 = follow the time zone of the browser that opens /settings */
int conf_timezone_auto(void);
/* changes whenever the time zone changes, so the goggles' clock gets set again */
unsigned conf_generation(void);
/* validate, save to metastreamer.txt, apply. NULL = ok, else the error */
const char *conf_set_timezone(const char *tz, int is_auto);
/* "set the goggles' clock again" from /settings */
void conf_request_clock_sync(void);
unsigned conf_clock_sync_generation(void);

/* Wi-Fi address: DHCP, or static ("192.168.1.50/24", gateway, DNS); metastreamer-wifi applies it */
int conf_net_static(void);
const char *conf_net_address(void);
const char *conf_net_gateway(void);
const char *conf_net_dns(void);
/* validate, save, apply (metastreamer-wifi net). NULL = ok, else the error */
const char *conf_set_network(int is_static, const char *address, const char *gateway, const char *dns);
#endif
