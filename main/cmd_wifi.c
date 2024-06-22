/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tinyusb.h"

#if CFG_TUD_NCM || CFG_TUD_ECM_RNDIS

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_smartconfig.h"
#include "esp_private/wifi.h"

#include "tinyusb_net.h"
#include "tinyusb.h"
#include "data_back.h"

static const char *TAG = "esp_network";

static TaskHandle_t Smart_Config_Handle = NULL;

static bool reconnect = true;
static bool wifi_start = false;
static bool smart_config = false;

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;
const int ESPTOUCH_DONE_BIT = BIT2;
esp_netif_t *ap_netif;
esp_netif_t *sta_netif;

DRAM_ATTR uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};
bool s_wifi_is_connected = false;

static esp_err_t pkt_wifi2usb(void *buffer, uint16_t len, void *eb);

static void scan_done_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    uint16_t sta_number = 0;
    uint8_t i;
    wifi_ap_record_t *ap_list_buffer;
    char *scan_buf = (char *)malloc(64);
    size_t length = 0;

    esp_wifi_scan_get_ap_num(&sta_number);
    if (!sta_number) {
        ESP_LOGE(TAG, "No AP found");
        length = sprintf(scan_buf, "\r\nNo AP found\r\n>");
        esp_data_back(scan_buf, length, ENABLE_FLUSH);
        free(scan_buf);
        return;
    }

    ap_list_buffer = malloc(sta_number * sizeof(wifi_ap_record_t));
    if (ap_list_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to malloc buffer to print scan results");
        length = sprintf(scan_buf, "\r\nFailed to malloc\r\n>");
        esp_data_back(scan_buf, length, ENABLE_FLUSH);
        free(scan_buf);
        return;
    }

    if (esp_wifi_scan_get_ap_records(&sta_number, (wifi_ap_record_t *)ap_list_buffer) == ESP_OK) {
        for (i = 0; i < sta_number; i++) {
            ESP_LOGI(TAG, "[%s][rssi=%d]", ap_list_buffer[i].ssid, ap_list_buffer[i].rssi);
            length = sprintf(scan_buf, "\r\n[%s][rssi=%d]", ap_list_buffer[i].ssid, ap_list_buffer[i].rssi);
            esp_data_back(scan_buf, length, DISABLE_FLUSH);
        }
    }
    length = sprintf(scan_buf, "\r\n>");
    esp_data_back(scan_buf, length, ENABLE_FLUSH);
    free(scan_buf);
    free(ap_list_buffer);
    ESP_LOGI(TAG, "sta scan done");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    char *wifi_event_buf = (char *)malloc(128);
    size_t length = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        uint8_t *tud_network_mac_address_dummy = tud_network_mac_address;
        esp_wifi_get_mac(ESP_IF_WIFI_STA, tud_network_mac_address_dummy);
        // esp_wifi_connect();
        wifi_start = true;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi STA disconnected");
        s_wifi_is_connected = false;
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, NULL);

        if (reconnect && tud_ready()) {
            ESP_LOGI(TAG, "sta disconnect, reconnect...");
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "sta disconnect");
        }
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        xEventGroupSetBits(wifi_event_group, DISCONNECTED_BIT);
        ESP_LOGI(TAG, "DISCONNECTED_BIT\r\n");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        if (smart_config == false) {
            ESP_LOGI(TAG, "Wi-Fi STA connected");
            esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, pkt_wifi2usb);
            s_wifi_is_connected = true;
            xEventGroupClearBits(wifi_event_group, DISCONNECTED_BIT);
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            ESP_LOGI(TAG, "CONNECTED_BIT\r\n");
        }
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        uint8_t ssid[33] = { 0 };
        uint8_t password[65] = { 0 };
        uint8_t rvd_data[33] = { 0 };

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;
        if (wifi_config.sta.bssid_set == true) {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s    PASSWORD:%s", ssid, password);
        length = sprintf(wifi_event_buf, "SSID:%s,PASSWORD:%s\r\n", ssid, password);
        esp_data_back(wifi_event_buf, length, ENABLE_FLUSH);

        if (evt->type == SC_TYPE_ESPTOUCH_V2) {
            ESP_ERROR_CHECK(esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)));
            ESP_LOGI(TAG, "RVD_DATA:");
            for (int i = 0; i < 33; i++) {
                printf("%02x ", rvd_data[i]);
            }
            printf("\n");
        }

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        ESP_LOGI(TAG, "Send ACK done");
        xEventGroupSetBits(wifi_event_group, ESPTOUCH_DONE_BIT);
        esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, pkt_wifi2usb);
        s_wifi_is_connected = true;
        xEventGroupClearBits(wifi_event_group, DISCONNECTED_BIT);
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
    free(wifi_event_buf);
}

uint32_t wifi_get_local_ip(void)
{
    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);
    esp_netif_t * netif = ap_netif;
    esp_netif_ip_info_t ip_info;
    wifi_mode_t mode;

    esp_wifi_get_mode(&mode);
    if (WIFI_MODE_STA == mode) {
        bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);
        if (bits & CONNECTED_BIT) {
            netif = sta_netif;
        } else {
            ESP_LOGE(TAG, "sta has no IP");
            return 0;
        }
    }

    esp_netif_get_ip_info(netif, &ip_info);
    return ip_info.ip.addr;
}

esp_err_t wifi_cmd_set_mode(char* mode)
{
    esp_err_t ret = ESP_FAIL;
    if (!strncmp(mode, "sta", strlen("sta"))) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ret = ESP_OK;
    } else if (!strncmp(mode, "ap", strlen("ap"))) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ret = ESP_OK;
    }

    return ret;
}

esp_err_t wifi_cmd_sta_join(const char* ssid, const char* pass)
{
    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);

    wifi_config_t wifi_config = { 0 };
    wifi_config.sta.pmf_cfg.capable = true;

    strlcpy((char*) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (pass) {
        strlcpy((char*) wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    }

    if (bits & CONNECTED_BIT) {
        reconnect = false;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        ESP_ERROR_CHECK(esp_wifi_disconnect());

        xEventGroupWaitBits(wifi_event_group, DISCONNECTED_BIT, 0, 1, 1000 / portTICK_PERIOD_MS);
    }

    reconnect = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_wifi_connect();

    EventBits_t status = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 5000 / portTICK_PERIOD_MS);

    if (status & CONNECTED_BIT) {
        ESP_LOGI(TAG, "connect success\n");

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Connect fail\n");
    reconnect = false;
    return ESP_FAIL;
}

esp_err_t wif_cmd_disconnect_wifi(void)
{
    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);
    if (bits & CONNECTED_BIT) {
        reconnect = false;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        ESP_ERROR_CHECK(esp_wifi_disconnect());

        xEventGroupWaitBits(wifi_event_group, DISCONNECTED_BIT, 0, 1, portTICK_PERIOD_MS);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t wifi_cmd_sta_scan(const char* ssid)
{
    wifi_scan_config_t scan_config = { 0 };
    scan_config.ssid = (uint8_t *) ssid;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_scan_start(&scan_config, false);

    return ESP_OK;
}

esp_err_t wifi_cmd_ap_set(const char* ssid, const char* pass)
{
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .max_connection = 4,
            .password = "",
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    reconnect = false;
    strlcpy((char*) wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    if (pass) {
        if (strlen(pass) != 0 && strlen(pass) < 8) {
            reconnect = true;
            ESP_LOGE(TAG, "password less than 8");
            return ESP_FAIL;
        }
        strlcpy((char*) wifi_config.ap.password, pass, sizeof(wifi_config.ap.password));
    }

    if (strlen(pass) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    return ESP_OK;
}

esp_err_t wifi_cmd_query(void)
{
    wifi_config_t cfg = {0};
    wifi_mode_t mode;
    char *query_buf = (char *)malloc(128);

    memset(&cfg, 0, sizeof(cfg));

    esp_wifi_get_mode(&mode);
    if (WIFI_MODE_AP == mode) {
        esp_wifi_get_config(WIFI_IF_AP, &cfg);
        ESP_LOGI(TAG, "AP mode, %s %s", cfg.ap.ssid, cfg.ap.password);
        size_t length = sprintf(query_buf, "AP mode:%s,%s\r\n", cfg.ap.ssid, cfg.ap.password);
        esp_data_back(query_buf, length, ENABLE_FLUSH);
    } else if (WIFI_MODE_STA == mode) {
        int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);
        if (bits & CONNECTED_BIT) {
            esp_wifi_get_config(WIFI_IF_STA, &cfg);
            ESP_LOGI(TAG, "STA mode: %s,%d,%d,%d", cfg.sta.ssid, cfg.sta.channel, cfg.sta.listen_interval, cfg.sta.threshold.authmode);
            size_t length = sprintf(query_buf, "STA mode:%s,%d,%d,%d\r\n", cfg.sta.ssid, cfg.sta.channel, cfg.sta.listen_interval, cfg.sta.threshold.authmode);
            esp_data_back(query_buf, length, ENABLE_FLUSH);
        } else {
            ESP_LOGI(TAG, "sta mode, disconnected");
        }
    } else {
        ESP_LOGI(TAG, "NULL mode");
        return ESP_FAIL;
    }
    free(query_buf);
    return ESP_OK;
}

static void smartconfig_task(void * param)
{
    EventBits_t uxBits;
    char *sm_buf = (char *)malloc(52);
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    wif_cmd_disconnect_wifi();

    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    while (1) {
        uxBits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
        if (uxBits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }

        if (uxBits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI(TAG, "smartconfig over");
            size_t length = sprintf(sm_buf, "OK\r\n>");
            esp_data_back(sm_buf, length, ENABLE_FLUSH);
            esp_smartconfig_stop();
            smart_config = false;
            ESP_LOGI(TAG, "free the buffer taken by smartconfig");
            free(sm_buf);
            vTaskDelete(NULL);
        }
    }
}

esp_err_t wifi_cmd_start_smart_config(void)
{
    if (wifi_start) {
        if (smart_config) {
            ESP_LOGE(TAG, "SmartConfig Task is Created\r\n");
            return ESP_FAIL;
        }
        xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, &Smart_Config_Handle);
        ESP_LOGI(TAG, "Smart Config Task Create Success\r\n");
        smart_config = true;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t wifi_cmd_stop_smart_config(void)
{
    if (smart_config) {
        ESP_LOGI(TAG, "stop smartconfig");
        esp_smartconfig_stop();
        smart_config = false;
        ESP_LOGI(TAG, "free the buffer taken by smartconfig");
        vTaskDelete(Smart_Config_Handle);
        ESP_LOGI(TAG, "delete OK\r\n");
        return ESP_OK;
    }
    return ESP_FAIL;
}

void wifi_buffer_free(void *buffer, void *ctx)
{
    esp_wifi_internal_free_rx_buffer(buffer);
}

esp_err_t wifi_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (s_wifi_is_connected) {
        esp_wifi_internal_tx(ESP_IF_WIFI_STA, buffer, len);
    }
    return ESP_OK;
}

static esp_err_t pkt_wifi2usb(void *buffer, uint16_t len, void *eb)
{
    if (tinyusb_net_send_sync(buffer, len, eb, portMAX_DELAY) != ESP_OK) {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    return ESP_OK;
}

/* Initialize Wi-Fi as sta and set scan method */
void initialise_wifi(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    static bool initialized = false;

    if (initialized) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        WIFI_EVENT_SCAN_DONE,
                                                        &scan_done_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_config_t wifi_config;
    esp_err_t err = esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Connecting to previously configured network: SSID: %s", wifi_config.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else {
        ESP_LOGI(TAG, "No previously stored Wi-Fi configuration found.");
        // Here you might prompt the user for new Wi-Fi credentials
        // or set a default configuration if desired.
    }
    initialized = true;
}

#endif /* CFG_TUD_NCM || CFG_TUD_ECM_RNDIS */
