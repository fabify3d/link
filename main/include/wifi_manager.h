#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include <string.h>

class WiFiManager
{
private:
    static const char *TAG;
    static const int WIFI_CONNECTED_BIT = BIT0;
    static const int WIFI_FAIL_BIT = BIT1;
    static const int MAX_RETRY = 5;

    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    EventGroupHandle_t wifi_event_group;
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    bool initialized;
    bool sta_connected;
    bool ap_started;
    bool auto_reconnect;
    int retry_num;

    esp_timer_handle_t backoff_timer;
    uint32_t backoff_delay_ms;

    static void backoff_timer_cb(void* arg) {
        WiFiManager* self = (WiFiManager*)arg;
        esp_wifi_connect();
    }

    char sta_ssid[32];
    char sta_password[64];
    char ap_ssid[32];
    char ap_password[64];

    static void event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
    {
        WiFiManager *self = (WiFiManager *)arg;

        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            self->sta_connected = false;
            if (self->auto_reconnect && self->retry_num < MAX_RETRY)
            {
                esp_wifi_connect();
                self->retry_num++;
                ESP_LOGI(TAG, "Retry connecting to AP... (%d/%d)", self->retry_num, MAX_RETRY);
            }
            else if (self->retry_num >= MAX_RETRY)
            {
                if (self->backoff_delay_ms == 0) self->backoff_delay_ms = 10000;
                else if (self->backoff_delay_ms < 60000) self->backoff_delay_ms *= 2;
                if (self->backoff_delay_ms > 60000) self->backoff_delay_ms = 60000;
                
                ESP_LOGI(TAG, "Backing off WiFi reconnect for %lu ms", self->backoff_delay_ms);
                if (self->backoff_timer) {
                    esp_timer_start_once(self->backoff_timer, self->backoff_delay_ms * 1000ULL);
                }
                self->retry_num = 0; // Reset for after backoff
                xEventGroupSetBits(self->wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGI(TAG, "Failed to connect to AP (temporarily)");
            }
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
            self->retry_num = 0;
            self->backoff_delay_ms = 0; // Reset backoff on success
            self->sta_connected = true;
            xEventGroupSetBits(self->wifi_event_group, WIFI_CONNECTED_BIT);
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
        {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d",
                     MAC2STR(event->mac), event->aid);
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
        {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d",
                     MAC2STR(event->mac), event->aid);
        }
    }

public:
    WiFiManager() : sta_netif(nullptr), ap_netif(nullptr), wifi_event_group(nullptr),
                    instance_any_id(nullptr), instance_got_ip(nullptr),
                    initialized(false), sta_connected(false), ap_started(false),
                    auto_reconnect(true), retry_num(0), backoff_timer(nullptr), backoff_delay_ms(0)
    {
        memset(sta_ssid, 0, sizeof(sta_ssid));
        memset(sta_password, 0, sizeof(sta_password));
        memset(ap_ssid, 0, sizeof(ap_ssid));
        memset(ap_password, 0, sizeof(ap_password));
    }

    ~WiFiManager()
    {
        if (backoff_timer) {
            esp_timer_stop(backoff_timer);
            esp_timer_delete(backoff_timer);
        }
        if (initialized)
        {
            disconnect();
            endAP();
            esp_wifi_deinit();
            esp_netif_deinit();
            if (wifi_event_group)
            {
                vEventGroupDelete(wifi_event_group);
            }
        }
    }

    esp_err_t begin(bool init_nvs = true)
    {
        if (initialized)
        {
            ESP_LOGW(TAG, "WiFi already initialized");
            return ESP_OK;
        }

        // Initialize NVS only if requested (required by WiFi driver for calibration data)
        // Set init_nvs to false if NVS is already initialized elsewhere (e.g., by storage_init())
        esp_err_t ret = ESP_OK;
        if (init_nvs)
        {
            ret = nvs_flash_init();
            if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
            {
                // NVS partition was truncated and needs to be erased
                ESP_LOGW(TAG, "NVS partition issue, erasing...");
                ret = nvs_flash_erase();
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
                    return ret;
                }
                // Retry init after erase
                ret = nvs_flash_init();
            }
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
                return ret;
            }
            ESP_LOGI(TAG, "NVS initialized successfully");
        }
        else
        {
            ESP_LOGI(TAG, "Skipping NVS init (assumed already initialized)");
        }

        ret = esp_netif_init();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
            return ret;
        }

        wifi_event_group = xEventGroupCreate();
        if (!wifi_event_group)
        {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_FAIL;
        }

        esp_timer_create_args_t timer_args = {};
        timer_args.callback = &WiFiManager::backoff_timer_cb;
        timer_args.arg = this;
        timer_args.name = "wifi_backoff";
        esp_timer_create(&timer_args, &backoff_timer);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set WiFi storage: %s", esp_err_to_name(ret));
            return ret;
        }

        initialized = true;
        ESP_LOGI(TAG, "WiFi Manager initialized");
        return ESP_OK;
    }

    esp_err_t connect(const char *ssid, const char *password, bool wait_for_connection = false, uint32_t timeout_ms = 10000)
    {
        if (!initialized)
        {
            ESP_LOGE(TAG, "WiFi not initialized. Call begin() first");
            return ESP_ERR_INVALID_STATE;
        }

        // Stop WiFi if already running to ensure clean state
        if (sta_connected || retry_num >= MAX_RETRY)
        {
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(100)); // Allow time for cleanup
        }

        // Clear event bits from previous connection attempts
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        if (strlen(ssid) > 31 || strlen(password) > 63) {
            ESP_LOGE(TAG, "WiFi credentials too long");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(sta_ssid, sizeof(sta_ssid), "%s", ssid);
        snprintf(sta_password, sizeof(sta_password), "%s", password);

        if (!sta_netif)
        {
            sta_netif = esp_netif_create_default_wifi_sta();
        }

        // Unregister old handlers if they exist
        if (instance_any_id)
        {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
            instance_any_id = nullptr;
        }
        if (instance_got_ip)
        {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
            instance_got_ip = nullptr;
        }

        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &event_handler, this, &instance_any_id);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &event_handler, this, &instance_got_ip);

        wifi_config_t wifi_config = {};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;

        esp_err_t ret;
        if (ap_started)
        {
            ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        }
        else
        {
            ret = esp_wifi_set_mode(WIFI_MODE_STA);
        }

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_wifi_start();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
            return ret;
        }

        retry_num = 0;

        // Only wait if explicitly requested
        if (wait_for_connection)
        {
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

            if (bits & WIFI_CONNECTED_BIT)
            {
                ESP_LOGI(TAG, "Connected to AP: %s", ssid);
                return ESP_OK;
            }
            else if (bits & WIFI_FAIL_BIT)
            {
                ESP_LOGE(TAG, "Failed to connect to AP: %s", ssid);
                return ESP_FAIL;
            }

            return ESP_ERR_TIMEOUT;
        }

        // Return immediately for async connection
        ESP_LOGI(TAG, "Connection initiated to: %s", ssid);
        return ESP_OK;
    }

    esp_err_t disconnect()
    {
        if (!sta_connected)
        {
            return ESP_OK;
        }

        esp_err_t ret = esp_wifi_disconnect();
        if (ret == ESP_OK)
        {
            sta_connected = false;
            ESP_LOGI(TAG, "Disconnected from AP");
        }
        return ret;
    }

    esp_err_t startAP(const char *ssid, const char *password,
                      uint8_t channel = 1, uint8_t max_connections = 4)
    {
        if (!initialized)
        {
            ESP_LOGE(TAG, "WiFi not initialized. Call begin() first");
            return ESP_ERR_INVALID_STATE;
        }

        if (ap_started)
        {
            ESP_LOGW(TAG, "AP already started");
            return ESP_OK;
        }

        if (strlen(ssid) > 31 || strlen(password) > 63) {
            ESP_LOGE(TAG, "AP credentials too long");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid);
        snprintf(ap_password, sizeof(ap_password), "%s", password);

        if (!ap_netif)
        {
            ap_netif = esp_netif_create_default_wifi_ap();
        }

        wifi_config_t ap_config = {};
        strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
        strncpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password));
        ap_config.ap.ssid_len = strlen(ssid);
        ap_config.ap.channel = channel;
        ap_config.ap.max_connection = max_connections;
        ap_config.ap.authmode = strlen(password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

        esp_err_t ret;
        if (sta_connected)
        {
            ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        }
        else
        {
            ret = esp_wifi_set_mode(WIFI_MODE_AP);
        }

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(ret));
            return ret;
        }

        if (!sta_connected)
        {
            ret = esp_wifi_start();
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
                return ret;
            }
        }

        ap_started = true;
        ESP_LOGI(TAG, "AP started: %s", ssid);
        return ESP_OK;
    }

    esp_err_t endAP()
    {
        if (!ap_started)
        {
            return ESP_OK;
        }

        esp_err_t ret;
        if (sta_connected)
        {
            ret = esp_wifi_set_mode(WIFI_MODE_STA);
        }
        else
        {
            ret = esp_wifi_stop();
        }

        if (ret == ESP_OK)
        {
            ap_started = false;
            ESP_LOGI(TAG, "AP stopped");
        }
        return ret;
    }

    bool getIP(char *ip_str, size_t len, bool station = true)
    {
        esp_netif_t *netif = station ? sta_netif : ap_netif;
        if (!netif)
            return false;

        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        {
            snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
            return true;
        }
        return false;
    }

    int8_t getRSSI()
    {
        if (!sta_connected)
            return 0;

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            return ap_info.rssi;
        }
        return 0;
    }

    void toggleReconnection(bool enable)
    {
        auto_reconnect = enable;
        ESP_LOGI(TAG, "Auto-reconnect %s", enable ? "enabled" : "disabled");
    }

    bool isReconnecting()
    {
        return auto_reconnect && !sta_connected && retry_num > 0;
    }

    bool isConnected()
    {
        return sta_connected;
    }

    bool isAPStarted()
    {
        return ap_started;
    }

    std::string getStatus()
    {
        std::string result = "";
        result += "Initialized: " + std::string(initialized ? "Yes" : "No") + "\n";
        result += "STA Connected: " + std::string(sta_connected ? "Yes" : "No") + "\n";

        if (sta_connected)
        {
            char ip[16];
            if (getIP(ip, sizeof(ip), true))
            {
                result += "STA IP: " + std::string(ip) + "\n";
            }

            result += "RSSI: " + std::to_string(getRSSI()) + " dBm" + "\n";
            result += "STA SSID: " + std::string(sta_ssid) + "\n";
        }

        result += "AP Started: " + std::string(ap_started ? "Yes" : "No") + "\n";
        if (ap_started)
        {
            char ap_ip[16];
            if (getIP(ap_ip, sizeof(ap_ip), false))
            {   
                result += "AP IP: " + std::string(ap_ip) + "\n";
            }

            result += "AP Password: " + std::string(ap_password[0] ? ap_password : "<None>") + "\n";
            result += "AP Channel: " + std::to_string(1) + "\n";         // Channel is fixed at 1 in startAP()
            result += "AP Max Connections: " + std::to_string(4) + "\n"; // Max connections fixed at 4 in startAP()
            result += "AP SSID: " + std::string(ap_ssid) + "\n";
        }

        result += "Auto-Reconnect: " + std::string(auto_reconnect ? "Enabled" : "Disabled") + "\n";
        result += "";

        return result;
    }

    void printStatus()
    {
        ESP_LOGI(TAG, "%s", getStatus().c_str());
    }
};

const char *WiFiManager::TAG = "WiFiManager";

#endif // WIFI_MANAGER_H