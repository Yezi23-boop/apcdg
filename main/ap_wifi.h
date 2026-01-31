#ifndef AP_WIFI_H
#define AP_WIFI_H
#include "wifi_manager.h"
void ap_wifi_init(p_wifi_state_callback f);

void ap_wifi_connect(void);

void ap_wifi_apcfg(void);
#endif // AP_WIFI_H