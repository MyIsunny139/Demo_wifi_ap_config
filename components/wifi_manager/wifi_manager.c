#include "wifi_manager.h"


#define MAX_CONNECT_RETRY 10
static int sta_connect_num = 0;

#define TAG "wifi_manager"

//当前STA连接状态
static bool wifi_sta_connected = false;

static p_wifi_state_cb wifi_callback = NULL;

esp_netif_t* ap_netif_ap = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) 
    {
        switch(event_id) 
        {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                if(wifi_sta_connected) 
                {
                    wifi_sta_connected = false;
                    if(wifi_callback) 
                    wifi_callback(WIFI_STATE_DISCONNECTED);
                }
                if(sta_connect_num<MAX_CONNECT_RETRY)
                {
                    esp_wifi_connect();
                    sta_connect_num++;
                }
                else
                {
                    if(wifi_callback) wifi_callback(WIFI_STATE_CONNECTION_FAILED);
                    sta_connect_num = 0;
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi Connected to AP");
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Sta device connected!");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Sta device disconnected!");
                break;
            
            default:
                break;
        }
    } 
    else if (event_base == IP_EVENT) 
    {
        switch(event_id) 
        {
            case IP_EVENT_STA_GOT_IP:
                ESP_LOGI(TAG, "Got IP Address");
                wifi_sta_connected = true;
                sta_connect_num = 0;
                if(wifi_callback) 
                wifi_callback(WIFI_STATE_CONNECTED);
                break;
            default:
                break;
        }
    }
}


void wifi_manager_init(p_wifi_state_cb f)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    
    ap_netif_ap = esp_netif_create_default_wifi_ap();


    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
                                                        
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_callback = f; 
    scan_semaphore = xSemaphoreCreateBinary(); //创建二进制信号量
    xSemaphoreGive(scan_semaphore); //初始化时释放信号量
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_start() );

}

void wifi_manager_connect(const char* ssid , const char* password)
{
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },

    };
    snprintf((char*)wifi_config.sta.ssid, 31, "%s", ssid);
    snprintf((char*)wifi_config.sta.password, 63, "%s", password);
    
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if(mode != WIFI_MODE_STA ) 
    {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    sta_connect_num = 0;

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    esp_wifi_start();

}



esp_err_t wifi_manager_ap(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if(mode == WIFI_MODE_APSTA)
    {
        return ESP_OK;
    }
    else
    {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        wifi_config_t wifi_config = {
            .ap = 
            {
                .ssid = "ESP32_AP",
                .ssid_len = 0,
                .channel = 5,
                .max_connection = 2,
                .authmode = WIFI_AUTH_WPA2_PSK,
            }
        };
        snprintf((char*)wifi_config.ap.ssid, 32, "%s", WIFI_AP_SSID);
        wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
        snprintf((char*)wifi_config.ap.password, 64, "%s", WIFI_AP_PASSWORD);

        esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config);
        
        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192,168,1,1);     //配置AP端IP地址
        IP4_ADDR(&ip_info.gw, 192,168,1,1);     //配置AP端网关地址
        IP4_ADDR(&ip_info.netmask, 255,255,255,0);  //配置AP端子网掩码

        esp_netif_dhcps_stop(ap_netif_ap);  //停止AP端DHCP服务
        esp_netif_set_ip_info(ap_netif_ap, &ip_info);   //设置AP端IP信息  
        esp_netif_dhcps_start(ap_netif_ap); //启动AP端DHCP服务

        return esp_wifi_start();
        
    }
}

static void wifi_scan_task(void* param)
{
    p_wifi_scan_cb f = (p_wifi_scan_cb)param;
    uint16_t ap_count = 0;
    uint16_t ap_num = 20;
    wifi_ap_record_t* ap_list = (wifi_ap_record_t*) malloc(sizeof(wifi_ap_record_t)*ap_num);
    esp_wifi_scan_start(NULL, true); //阻塞式扫描
    esp_wifi_scan_get_ap_num(&ap_count);
    esp_wifi_scan_get_ap_records(&ap_num, ap_list); //获取扫描到的AP数量
    ESP_LOGI(TAG, "Total APs scanned = %d,actual ap number :%d", ap_count, ap_num);
    if(f)
    {
        f(ap_num, ap_list);
    }
    free(ap_list);
    xSemaphoreGive(scan_semaphore); //释放信号量
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_scan(p_wifi_scan_cb f)
{
    if(pdTRUE == xSemaphoreTake(scan_semaphore, 0)) //尝试获取信号量
    {
        esp_wifi_clear_ap_list();
        return xTaskCreatePinnedToCore(wifi_scan_task, "wifi_scan_task", 8192, f, 5, NULL, 1);
    }
   
    return ESP_OK;
}