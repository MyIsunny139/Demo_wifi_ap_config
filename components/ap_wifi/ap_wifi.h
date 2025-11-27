#ifndef _AP_WIFI_H_
#define _AP_WIFI_H_


#include "main.h"



void ap_wifi_init(p_wifi_state_cb f); //初始化整个wifi功能

void ap_wifi_connect(void); //用指定ssid和密码连接wifi

void ap_wifi_apcfg(void);//配置为ap模式 启动ap配网



#endif // _AP_WIFI_H_