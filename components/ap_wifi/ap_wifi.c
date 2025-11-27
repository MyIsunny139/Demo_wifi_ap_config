#include <stdio.h>
#include "ap_wifi.h"

#define SPIFF_MOUNT "/spiffs"
#define HTML_PATH "spiffs:/apcfg.html"

static char* html_code = NULL;

static char *init_web_page_buffer(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFF_MOUNT,         //挂载路径
        .max_files = 3, //最大打开文件数
        .format_if_mount_failed = false, //不格式化
        .partition_label = NULL, //使用第一个spiffs分区
    };
    esp_vfs_spiffs_register(&conf);
    struct stat st;
    if(stat(HTML_PATH, &st))
    {
        printf("File not found\n");
        return NULL;
    }
    char* buf = (char*)malloc(st.st_size + 1);
    memset(buf, 0, st.st_size + 1);
    FILE* fp = fopen(HTML_PATH, "r");
    if(fp)
    {
        if(fread(buf, st.st_size, 1, fp)==0)
        {
            free(buf);
            buf = NULL;
        }
        fclose(fp);
    }
    else
    {
        free(buf);
        buf = NULL;
    }
    return buf;
}

void ap_wifi_init(p_wifi_state_cb f)
{
    wifi_manager_init(f);
    html_code = init_web_page_buffer();
}

void ap_wifi_connect(void)
{
    wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD);
}

static void ws_receive_callback(uint8_t* payload, int len)
{
    cJSON* root = cJSON_Parse(payload);
    if(root)
    {
        cJSON* scan_js = cJSON_GetObjectItem(root, "scan");
        cJSON* ssid_js = cJSON_GetObjectItem(root, "ssid");
        cJSON* password_js = cJSON_GetObjectItem(root, "password");
        if(scan_js)
        {
            char* scan_value = cJSON_GetStringValue(scan_js);
            if((strcmp(scan_value, "start")==0))
            {
                //开始扫描
                wifi_manager_scan(NULL);
            }
        }
        if(ssid_js && password_js)
        {
            char* ssid_value = cJSON_GetStringValue(ssid_js);
            char* password_value = cJSON_GetStringValue(password_js);
            if((ssid_value != NULL) && (password_value != NULL))
            {
                //连接指定的wifi
                wifi_manager_connect(ssid_value, password_value);
            }
        }
    }
}

void ap_wifi_apcfg(void)
{
    wifi_manager_ap();
    ws_server_config_t ws_cfg =
    {
        .html_code = html_code,
        .receive_fn = ws_receive_callback,
    };
    web_ws_server_start(&ws_cfg);
}