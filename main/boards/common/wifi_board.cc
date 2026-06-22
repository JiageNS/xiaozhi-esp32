#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#ifdef CONFIG_XIAOLU_MODE
#include "local_storage_protocol.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <lwip/ip4_addr.h>
#include <cJSON.h>
#include <utility>
#include <cstring>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include <dns_server.h>
#include "afsk_demod.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
            // Stop provisioning API and SoftAP if they were running
            StopProvisioningApi();
            StopSoftApOnly();
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#ifdef CONFIG_XIAOLU_MODE
            {
                // Keep BluFi alive if no device_token yet (waiting for App binding)
                Settings xiaolu_settings("xiaolu", false);
                std::string token = xiaolu_settings.GetString("device_token");
                if (!token.empty()) {
                    Blufi::GetInstance().SafeDeinit();
                } else {
                    // 尝试自动注册
                    ESP_LOGI(TAG, "XiaoLu: no token, attempting auto-register...");
                    xTaskCreate(AutoRegisterTask, "xl_reg", 8192, nullptr, 3, nullptr);
                }
            }
#else
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
#endif
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
#ifdef CONFIG_XIAOLU_MODE
            {
                // Check if token was provided during provisioning
                Settings xiaolu_settings("xiaolu", false);
                std::string token = xiaolu_settings.GetString("device_token");
                if (token.empty()) {
                    // No token yet — try auto-register with server
                    ESP_LOGI(TAG, "XiaoLu: no token after provisioning, attempting auto-register...");
                    xTaskCreate(AutoRegisterTask, "xl_reg", 8192, nullptr, 3, nullptr);
                } else {
                    ESP_LOGI(TAG, "XiaoLu: token found after provisioning, ready to go");
                }
            }
#endif
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            StopProvisioningApi();
            StopSoftApOnly();
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    // Start SoftAP without captive portal (no DNS hijack, no web page)
    // This prevents iOS from auto-opening the "受限 Wi-Fi" browser sheet.
    // All provisioning is done via our custom API on port 81.
    StartSoftApOnly();

    // Start custom provisioning API server on port 81
    StartProvisioningApi();

    // Show config prompt
    Application::GetInstance().Schedule([this]() {
        std::string hint = "请在手机 WiFi 设置中连接热点: ";
        hint += softap_ssid_;
        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    // Start acoustic provisioning task
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    ESP_LOGI(TAG, "Starting acoustic WiFi provisioning, channels: %d", channel);

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    // 已经在配网模式，避免重入
    if (state == kDeviceStateWifiConfiguring) {
        ESP_LOGI(TAG, "Already in WiFi config mode, ignoring");
        return;
    }

#ifdef CONFIG_XIAOLU_MODE
    // 必须先把心跳同步停掉，否则下面 StopStation 会把 SSL 握手中的 socket
    // 一刀切，心跳线程在 mbedtls 内部跑到 NULL callback 直接 panic。
    app.StopHeartbeat();
#endif

    // 任何运行态都允许进配网：先关音频通道、清 protocol，再延迟切到 SoftAP
    if (state != kDeviceStateStarting) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking/recording to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

// ─── SoftAP without Captive Portal ───────────────────────────────────────────
// Opens a plain SoftAP hotspot (no DNS hijack, no web page).
// iOS will NOT auto-open the "受限 Wi-Fi" browser sheet.

void WifiBoard::StartSoftApOnly() {
    // Generate SSID from MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "XiaoLu-%02X%02X", mac[4], mac[5]);
    softap_ssid_ = ssid;

    // Create AP netif if not already created
    static esp_netif_t* ap_netif = nullptr;
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    // Set IP
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    // Configure AP
    wifi_config_t wifi_config = {};
    strlcpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    // Ensure WiFi is stopped before switching mode
    esp_wifi_stop();

    // Start WiFi in AP mode
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    // Start DNS server — resolves ALL domains to 192.168.4.1
    // This is critical: without it, iOS resolves captive.apple.com via cellular,
    // never hits our /hotspot-detect.html handler, and marks WiFi as "no internet".
    dns_server_ = std::make_unique<DnsServer>();
    dns_server_->Start(ip_info.ip);

    ESP_LOGI(TAG, "SoftAP started: %s (with DNS hijack, no captive portal page)", ssid);
}

void WifiBoard::StopSoftApOnly() {
    if (dns_server_) {
        dns_server_->Stop();
        dns_server_.reset();
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    softap_ssid_.clear();
    ESP_LOGI(TAG, "SoftAP stopped");
}

// ─── Provisioning API (port 81) ───────────────────────────────────────────────
// Provides /device-info and /provision endpoints for App communication during SoftAP config mode.

void WifiBoard::StartProvisioningApi() {
    if (provision_server_) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&provision_server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning API server: %s", esp_err_to_name(err));
        return;
    }

    // GET /device-info — returns device_id and mac
    static httpd_uri_t device_info_uri = {
        .uri = "/device-info",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            char device_id[32];
            snprintf(device_id, sizeof(device_id), "xiaolu_%02x%02x%02x%02x%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            char resp[128];
            snprintf(resp, sizeof(resp),
                     "{\"device_id\":\"%s\",\"firmware\":\"2.2.6\",\"hardware\":\"v2\"}",
                     device_id);

            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &device_info_uri);

    // GET /scan-wifi — scan nearby WiFi networks and return list
    static httpd_uri_t scan_wifi_uri = {
        .uri = "/scan-wifi",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            // Need to temporarily switch to STA mode for scanning, then back to AP
            // Actually ESP32 in AP mode can still scan (AP+STA coexist briefly)
            wifi_mode_t current_mode;
            esp_wifi_get_mode(&current_mode);
            
            // Switch to APSTA mode to allow scanning while keeping AP alive
            if (current_mode == WIFI_MODE_AP) {
                esp_wifi_set_mode(WIFI_MODE_APSTA);
            }

            wifi_scan_config_t scan_config = {};
            scan_config.show_hidden = false;
            scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_config.scan_time.active.min = 100;
            scan_config.scan_time.active.max = 300;

            esp_err_t err = esp_wifi_scan_start(&scan_config, true);
            if (err != ESP_OK) {
                // Restore mode
                if (current_mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                httpd_resp_send(req, "{\"networks\":[]}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);
            if (ap_count > 20) ap_count = 20;

            wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (!ap_records) {
                esp_wifi_scan_stop();
                if (current_mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
                httpd_resp_send(req, "{\"networks\":[]}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            esp_wifi_scan_get_ap_records(&ap_count, ap_records);

            // Restore original mode
            if (current_mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);

            // Build JSON response
            char* json_buf = (char*)malloc(2048);
            if (!json_buf) { free(ap_records); httpd_resp_send(req, "{\"networks\":[]}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
            
            int offset = snprintf(json_buf, 2048, "{\"networks\":[");
            int added = 0;
            for (int i = 0; i < ap_count && offset < 1900; i++) {
                if (ap_records[i].ssid[0] == '\0') continue;
                // Skip duplicates
                bool dup = false;
                for (int j = 0; j < i; j++) {
                    if (strcmp((char*)ap_records[j].ssid, (char*)ap_records[i].ssid) == 0) { dup = true; break; }
                }
                if (dup) continue;
                
                if (added > 0) offset += snprintf(json_buf + offset, 2048 - offset, ",");
                bool is_open = (ap_records[i].authmode == WIFI_AUTH_OPEN);
                offset += snprintf(json_buf + offset, 2048 - offset,
                    "{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                    (char*)ap_records[i].ssid, ap_records[i].rssi, is_open ? "false" : "true");
                added++;
            }
            snprintf(json_buf + offset, 2048 - offset, "]}");

            free(ap_records);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_send(req, json_buf, HTTPD_RESP_USE_STRLEN);
            free(json_buf);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &scan_wifi_uri);

    // OPTIONS /scan-wifi — CORS preflight
    static httpd_uri_t scan_wifi_options_uri = {
        .uri = "/scan-wifi",
        .method = HTTP_OPTIONS,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
            httpd_resp_send(req, "", 0);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &scan_wifi_options_uri);

    // POST /provision — receives WiFi creds + device_token in one shot
    // Body: {"ssid": "...", "password": "...", "device_token": "..."}
    static httpd_uri_t provision_uri = {
        .uri = "/provision",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            // Read body
            int total_len = req->content_len;
            if (total_len <= 0 || total_len > 1024) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
                return ESP_FAIL;
            }
            char* buf = (char*)malloc(total_len + 1);
            if (!buf) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
                return ESP_FAIL;
            }
            int received = 0;
            while (received < total_len) {
                int ret = httpd_req_recv(req, buf + received, total_len - received);
                if (ret <= 0) {
                    free(buf);
                    httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timeout");
                    return ESP_FAIL;
                }
                received += ret;
            }
            buf[total_len] = '\0';

            // Parse JSON
            cJSON* root = cJSON_Parse(buf);
            free(buf);
            if (!root) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
                return ESP_FAIL;
            }

            cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
            cJSON* pwd_item = cJSON_GetObjectItem(root, "password");
            cJSON* token_item = cJSON_GetObjectItem(root, "device_token");
            cJSON* child_id_item = cJSON_GetObjectItem(root, "child_id");

            if (!cJSON_IsString(ssid_item) || !ssid_item->valuestring[0]) {
                cJSON_Delete(root);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"success\":false,\"error\":\"Missing ssid\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            std::string ssid = ssid_item->valuestring;
            std::string password = (cJSON_IsString(pwd_item) && pwd_item->valuestring) ? pwd_item->valuestring : "";

            // Save device_token to NVS if provided
            if (cJSON_IsString(token_item) && token_item->valuestring && token_item->valuestring[0]) {
                Settings xiaolu_settings("xiaolu", true);
                xiaolu_settings.SetString("device_token", token_item->valuestring);
                ESP_LOGI(TAG, "Provisioning: saved device_token to NVS");
            }

            // Save child_id to NVS if provided (used during self-register)
            if (cJSON_IsString(child_id_item) && child_id_item->valuestring && child_id_item->valuestring[0]) {
                Settings xiaolu_settings("xiaolu", true);
                xiaolu_settings.SetString("child_id", child_id_item->valuestring);
                ESP_LOGI(TAG, "Provisioning: saved child_id=%s to NVS", child_id_item->valuestring);
            }

            cJSON_Delete(root);

            // Save WiFi credentials
            auto& ssid_manager = SsidManager::GetInstance();
            ssid_manager.AddSsid(ssid, password);
            ESP_LOGI(TAG, "Provisioning: saved WiFi creds for SSID=%s", ssid.c_str());

            // Send success response before connecting (connection will drop SoftAP)
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);

            // Schedule reboot after HTTP response is flushed.
            // We previously tried Stop+Start station here, but on devices that
            // booted directly into config mode (no prior station session) the
            // event handler chain ends up in an inconsistent state and the new
            // station never scans / never connects ("Haven't to connect to a
            // suitable AP now!" loop). A clean restart guarantees the normal
            // boot path runs and picks up the saved credentials.
            xTaskCreate([](void*) {
                vTaskDelay(pdMS_TO_TICKS(800)); // Let HTTP response flush
                ESP_LOGI(TAG, "Provisioning complete, rebooting to connect new WiFi...");
                esp_restart();
            }, "prov_reboot", 4096, nullptr, 3, nullptr);

            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &provision_uri);

    // OPTIONS /provision — CORS preflight
    static httpd_uri_t provision_options_uri = {
        .uri = "/provision",
        .method = HTTP_OPTIONS,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
            httpd_resp_send(req, "", 0);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &provision_options_uri);

    // OPTIONS /device-info — CORS preflight
    static httpd_uri_t device_info_options_uri = {
        .uri = "/device-info",
        .method = HTTP_OPTIONS,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
            httpd_resp_send(req, "", 0);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &device_info_options_uri);

    // ─── Captive Portal spoofing ─────────────────────────────────────────────
    // Respond to iOS/Android connectivity checks with "Success" so the OS
    // thinks this WiFi has internet. This prevents iOS from:
    //   1. Popping the "受限 Wi-Fi" browser sheet
    //   2. Downgrading WiFi and routing traffic to cellular
    // Without this, fetch('http://192.168.4.1/...') from the App times out.

    static const char* APPLE_SUCCESS_HTML =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";

    // Apple captive portal detection
    static httpd_uri_t apple_hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, APPLE_SUCCESS_HTML, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &apple_hotspot);

    static httpd_uri_t apple_success = {
        .uri = "/library/test/success.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, APPLE_SUCCESS_HTML, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &apple_success);

    // Android captive portal detection (expects HTTP 204)
    static httpd_uri_t android_generate204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_send(req, nullptr, 0);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &android_generate204);

    // Windows NCSI
    static httpd_uri_t windows_ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &windows_ncsi);

    // ─── WiFi Configuration Web Page (GET /) ─────────────────────────────────
    // A simple HTML page that lets users configure WiFi via browser.
    // This works even when iOS/Android blocks App HTTP requests over SoftAP.
    static const char* WIFI_CONFIG_HTML = R"rawhtml(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小鹿豆 WiFi 配置</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#f5f5f7;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.card{background:#fff;border-radius:16px;padding:32px 24px;max-width:360px;width:100%;box-shadow:0 2px 12px rgba(0,0,0,.08)}
h1{font-size:20px;text-align:center;margin-bottom:4px;color:#1d1d1f}
.sub{font-size:13px;color:#86868b;text-align:center;margin-bottom:20px}
label{display:block;font-size:14px;color:#1d1d1f;margin-bottom:6px;margin-top:16px}
input,select{width:100%;padding:12px;border:1px solid #d2d2d7;border-radius:10px;font-size:16px;outline:none;background:#fff}
input:focus,select:focus{border-color:#34c759}
.btn{width:100%;padding:14px;background:#34c759;color:#fff;border:none;border-radius:12px;font-size:16px;font-weight:600;margin-top:24px;cursor:pointer}
.btn:active{opacity:.8}
.btn:disabled{background:#ccc}
.btn2{background:#007aff;margin-top:12px}
.msg{text-align:center;margin-top:16px;font-size:14px;color:#34c759}
.err{color:#ff3b30}
.note{font-size:12px;color:#86868b;margin-top:8px;text-align:center}
.wifi-list{max-height:200px;overflow-y:auto;margin-top:8px}
.wifi-item{display:flex;align-items:center;padding:12px;border-bottom:1px solid #f0f0f0;cursor:pointer}
.wifi-item:active{background:#f5f5f7}
.wifi-item .name{flex:1;font-size:15px;color:#1d1d1f}
.wifi-item .signal{font-size:12px;color:#86868b}
.wifi-item .lock{font-size:12px;margin-right:6px}
.manual-link{display:block;text-align:center;margin-top:12px;font-size:13px;color:#007aff;cursor:pointer}
#scan-status{text-align:center;padding:20px;color:#86868b;font-size:14px}
</style></head><body>
<div class="card">
<h1>小鹿豆 WiFi 配置</h1>
<p class="sub">选择您的家庭 WiFi（仅支持 2.4GHz）</p>
<div id="scan-section">
<div id="scan-status">正在扫描附近 WiFi...</div>
<div id="wifi-list" class="wifi-list"></div>
<button class="btn btn2" id="rescan-btn" style="display:none" onclick="scanWifi()">重新扫描</button>
<span class="manual-link" id="manual-link" onclick="showManual()">手动输入 WiFi 名称</span>
</div>
<form id="f" style="display:none">
<label>WiFi 名称</label><input id="ssid" required placeholder="输入 WiFi 名称">
<label>WiFi 密码</label><input id="pwd" type="password" placeholder="输入密码（无密码留空）">
<button class="btn" type="submit">连接 WiFi</button>
<span class="manual-link" onclick="showScan()">返回 WiFi 列表</span>
</form>
<div id="msg"></div>
<p class="note">配置成功后设备将自动连接 WiFi</p>
</div>
<script>
var selectedSecure=true;
function scanWifi(){
  var s=document.getElementById('scan-status');
  var l=document.getElementById('wifi-list');
  var rb=document.getElementById('rescan-btn');
  s.textContent='正在扫描...';s.style.display='block';l.innerHTML='';rb.style.display='none';
  fetch('/scan-wifi').then(r=>r.json()).then(d=>{
    s.style.display='none';rb.style.display='block';
    if(!d.networks||d.networks.length===0){s.textContent='未发现 WiFi，请确认设备已开机';s.style.display='block';return;}
    d.networks.sort((a,b)=>b.rssi-a.rssi);
    d.networks.forEach(n=>{
      var div=document.createElement('div');div.className='wifi-item';
      var sig=n.rssi>-50?'强':n.rssi>-70?'中':'弱';
      div.innerHTML='<span class="lock">'+(n.secure?'🔒':'')+'</span><span class="name">'+n.ssid+'</span><span class="signal">'+sig+' ('+n.rssi+')</span>';
      div.onclick=function(){selectWifi(n.ssid,n.secure);};
      l.appendChild(div);
    });
  }).catch(()=>{s.textContent='扫描失败，请重试';s.style.display='block';rb.style.display='block';});
}
function selectWifi(ssid,secure){
  document.getElementById('ssid').value=ssid;
  selectedSecure=secure;
  document.getElementById('scan-section').style.display='none';
  document.getElementById('f').style.display='block';
  if(!secure)document.getElementById('pwd').placeholder='此网络无密码';
}
function showManual(){
  document.getElementById('scan-section').style.display='none';
  document.getElementById('f').style.display='block';
  document.getElementById('ssid').value='';
}
function showScan(){
  document.getElementById('f').style.display='none';
  document.getElementById('scan-section').style.display='block';
}
document.getElementById('f').onsubmit=async function(e){
  e.preventDefault();
  var btn=this.querySelector('.btn');
  var msg=document.getElementById('msg');
  btn.disabled=true;btn.textContent='连接中...';msg.className='msg';msg.textContent='';
  try{
    var r=await fetch('/provision',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:document.getElementById('ssid').value,password:document.getElementById('pwd').value})});
    var d=await r.json();
    if(d.success!==false){msg.className='msg';msg.textContent='配置成功！设备正在连接 WiFi...';}
    else{msg.className='msg err';msg.textContent=d.error||'配置失败';btn.disabled=false;btn.textContent='连接 WiFi';}
  }catch(ex){msg.className='msg err';msg.textContent='通信失败，请确认已连接 XiaoLu 热点';btn.disabled=false;btn.textContent='连接 WiFi';}
};
scanWifi();
</script></body></html>)rawhtml";

    static httpd_uri_t wifi_config_page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, WIFI_CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(provision_server_, &wifi_config_page);

    ESP_LOGI(TAG, "Provisioning API started on port 80 (with web config page)");
}

void WifiBoard::StopProvisioningApi() {
    if (provision_server_) {
        httpd_stop(provision_server_);
        provision_server_ = nullptr;
        ESP_LOGI(TAG, "Provisioning API stopped");
    }
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

#ifdef CONFIG_XIAOLU_MODE
void WifiBoard::AutoRegisterTask(void* arg) {
    // 等待网络稳定
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 获取 MAC 地址作为 device_id
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[32];
    snprintf(device_id, sizeof(device_id), "xiaolu_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI("WifiBoard", "Auto-register: device_id=%s", device_id);

    // 构建注册请求 body
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id", device_id);
    cJSON_AddStringToObject(body, "mac_address", device_id + 7);  // skip "xiaolu_"
    cJSON_AddStringToObject(body, "firmware_version", "1.0.0");
    cJSON_AddStringToObject(body, "hardware_version", "bread-compact-wifi");
    char* json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    int json_len = strlen(json_str);

    // 发送注册请求 (使用 open/write/read 模式以获取响应体)
    std::string url = std::string(CONFIG_XIAOLU_SERVER_URL) + "/self-register";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, json_len);
    if (err != ESP_OK) {
        ESP_LOGW("WifiBoard", "Auto-register: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(json_str);
        vTaskDelete(nullptr);
        return;
    }

    // 写入请求体
    esp_http_client_write(client, json_str, json_len);
    free(json_str);

    // 读取响应头
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI("WifiBoard", "Register response: status=%d content_len=%d", status, content_len);

    if (status == 200 && content_len > 0 && content_len < 1024) {
        char* resp_buf = (char*)malloc(content_len + 1);
        if (resp_buf) {
            int read_len = esp_http_client_read(client, resp_buf, content_len);
            if (read_len > 0) {
                resp_buf[read_len] = '\0';
                ESP_LOGI("WifiBoard", "Register body: %s", resp_buf);

                cJSON* root = cJSON_Parse(resp_buf);
                if (root) {
                    cJSON* data = cJSON_GetObjectItem(root, "data");
                    if (data) {
                        cJSON* token_item = cJSON_GetObjectItem(data, "device_token");
                        if (token_item && token_item->valuestring) {
                            // 保存 device_token 到 NVS
                            Settings settings("xiaolu", true);
                            settings.SetString("device_token", token_item->valuestring);
                            ESP_LOGI("WifiBoard", "Auto-register SUCCESS! Token saved.");

                            // 在主线程启动录音
                            Application::GetInstance().Schedule([]() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
                                Blufi::GetInstance().deinit();
#endif

                                auto& app = Application::GetInstance();
                                app.InitializeProtocol();
                                Settings s("xiaolu", false);
                                std::string t = s.GetString("device_token");
                                auto* proto = static_cast<LocalStorageProtocol*>(app.GetProtocol());
                                proto->SetServerConfig(CONFIG_XIAOLU_SERVER_URL, t);
                                app.StartProtocolAndRecord();
                            });
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            free(resp_buf);
        }
    } else {
        ESP_LOGW("WifiBoard", "Auto-register: bad response status=%d len=%d", status, content_len);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(nullptr);
}
#endif
