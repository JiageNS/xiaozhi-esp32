#include "xiaolu_heartbeat.h"
#include "xiaolu_time.h"
#include "settings.h"
#include "ssid_manager.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <cJSON.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "XiaoLuHB"

XiaoLuHeartbeat::XiaoLuHeartbeat() {}

XiaoLuHeartbeat::~XiaoLuHeartbeat() {
    Stop();
}

void XiaoLuHeartbeat::SetServerConfig(const std::string& server_url, const std::string& device_token) {
    server_url_ = server_url;
    device_token_ = device_token;
}

void XiaoLuHeartbeat::Start(int interval_sec) {
    if (running_) return;
    running_ = true;

    // Load last processed command id from NVS so we don't replay history
    LoadLastProcessedId();

    // 启动心跳任务（独立线程，每 interval_sec 秒轮询一次）
    xTaskCreate(TaskEntry, "xl_hb", 8192, this, 3, &task_handle_);
    ESP_LOGI(TAG, "Heartbeat task started, interval=%ds (last_cmd_id=%d)",
             interval_sec, last_processed_id_);
}

void XiaoLuHeartbeat::Stop() {
    if (!running_) return;
    running_ = false;
    // 同步等待任务退出，避免主线程随后拆 WiFi 时 SSL 握手中崩溃
    // 最多等 8 秒（HTTP timeout 是 5 秒，留点余量）
    for (int i = 0; i < 80 && task_handle_ != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Heartbeat task didn't exit in 8s, leaking handle");
    }
}

void XiaoLuHeartbeat::TaskEntry(void* arg) {
    auto* self = static_cast<XiaoLuHeartbeat*>(arg);
    // 等 WiFi 稳定
    vTaskDelay(pdMS_TO_TICKS(8000));

    while (self->running_) {
        self->DoHeartbeat();
        // 拆成 6 个 500ms，让 Stop() 能快速生效
        for (int i = 0; i < 6 && self->running_; i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    self->task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void XiaoLuHeartbeat::DoHeartbeat() {
    if (server_url_.empty() || device_token_.empty()) return;

    // WiFi 没连上就别去打 HTTP 了，避免一堆 getaddrinfo 报错刷屏
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return;
    }
    wifi_rssi_ = ap_info.rssi;

    // 构建 JSON body
    cJSON* body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "battery", battery_);
    cJSON_AddNumberToObject(body, "storage_used_mb", storage_used_mb_);
    cJSON_AddNumberToObject(body, "storage_total_mb", storage_total_mb_);
    cJSON_AddBoolToObject(body, "is_recording", is_recording_);
    cJSON_AddBoolToObject(body, "is_charging", false);
    cJSON_AddNumberToObject(body, "wifi_rssi", wifi_rssi_);
    cJSON_AddStringToObject(body, "firmware_version", firmware_version_.c_str());
    cJSON_AddNumberToObject(body, "last_processed_command_id", last_processed_id_);
    if (upload_active_) {
        cJSON* upload = cJSON_AddObjectToObject(body, "upload_status");
        cJSON_AddNumberToObject(upload, "current_file", upload_current_);
        cJSON_AddNumberToObject(upload, "total_files", upload_total_);
        cJSON_AddNumberToObject(upload, "file_percent", upload_percent_);
    }
    if (!pending_acks_.empty()) {
        cJSON* arr = cJSON_AddArrayToObject(body, "acked_command_ids");
        for (int id : pending_acks_) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(id));
        }
    }
    char* json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    int json_len = strlen(json_str);

    // 发送 HTTP POST (使用 open/write/read 模式获取响应体)
    std::string url = server_url_ + "/heartbeat";
    std::string auth = "Bearer " + device_token_;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth.c_str());

    esp_err_t err = esp_http_client_open(client, json_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(json_str);
        return;
    }

    esp_http_client_write(client, json_str, json_len);
    free(json_str);

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status == 200 && content_len > 0 && content_len < 2048) {
        // Server received our acks; clear pending list
        pending_acks_.clear();

        char* resp = (char*)malloc(content_len + 1);
        if (resp) {
            int read_len = esp_http_client_read(client, resp, content_len);
            if (read_len > 0) {
                resp[read_len] = '\0';
                ParseResponse(resp, read_len);
            }
            free(resp);
        }
    } else if (status == 200) {
        // 200 but no body: still treat as ack received
        pending_acks_.clear();
    } else if (status == 401) {
        ESP_LOGW(TAG, "Heartbeat: 401 Unauthorized — token invalid, attempting re-register");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        DoReRegister();
        return;
    } else if (status != 200) {
        ESP_LOGW(TAG, "Heartbeat: status=%d", status);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void XiaoLuHeartbeat::ParseResponse(const char* json, int len) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data) { cJSON_Delete(root); return; }

    // 同步服务器时间（YYYY-MM-DDTHH:MM:SS 北京时区）
    cJSON* server_time = cJSON_GetObjectItem(data, "server_time");
    if (server_time && cJSON_IsString(server_time) && server_time->valuestring) {
        XiaoLuTime::SyncFromServerTime(server_time->valuestring);
    }

    // 解析命令
    cJSON* commands = cJSON_GetObjectItem(data, "commands");
    if (commands && cJSON_IsArray(commands)) {
        int count = cJSON_GetArraySize(commands);
        for (int i = 0; i < count; i++) {
            cJSON* cmd = cJSON_GetArrayItem(commands, i);
            if (!cmd) continue;

            DeviceCommand dc;
            cJSON* id_item = cJSON_GetObjectItem(cmd, "id");
            cJSON* cmd_item = cJSON_GetObjectItem(cmd, "command");
            cJSON* params_item = cJSON_GetObjectItem(cmd, "params");

            dc.id = id_item ? id_item->valueint : 0;
            dc.command = cmd_item && cmd_item->valuestring ? cmd_item->valuestring : "";
            if (params_item) {
                char* p = cJSON_PrintUnformatted(params_item);
                dc.params = p ? p : "{}";
                if (p) free(p);
            } else {
                dc.params = "{}";
            }

            if (!dc.command.empty()) {
                // Idempotent: skip commands we've already executed.
                // Server may resend the same id if our previous ack was lost,
                // or replay the whole queue after a reboot before TLS is up.
                if (dc.id > 0 && dc.id <= last_processed_id_) {
                    ESP_LOGI(TAG, "Skip stale command: %s (id=%d, last=%d)",
                             dc.command.c_str(), dc.id, last_processed_id_);
                    // Still ack so server stops resending
                    pending_acks_.push_back(dc.id);
                    continue;
                }

                ESP_LOGI(TAG, ">>> Command: %s (id=%d)", dc.command.c_str(), dc.id);
                if (command_callback_) {
                    command_callback_(dc);
                }

                // Mark as processed and queue ack for next heartbeat
                if (dc.id > 0) {
                    pending_acks_.push_back(dc.id);
                    if (dc.id > last_processed_id_) {
                        SaveLastProcessedId(dc.id);
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
}

void XiaoLuHeartbeat::DoReRegister() {
    if (server_url_.empty()) return;

    // Build device_id from MAC if not set
    std::string dev_id = device_id_;
    if (dev_id.empty()) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char buf[32];
        snprintf(buf, sizeof(buf), "xiaolu_%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        dev_id = buf;
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id", dev_id.c_str());
    cJSON_AddStringToObject(body, "mac_address", dev_id.c_str());
    cJSON_AddStringToObject(body, "firmware_version", firmware_version_.c_str());
    cJSON_AddStringToObject(body, "hardware_version", "bread-compact-wifi");
    char* json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    int json_len = strlen(json_str);

    std::string url = server_url_ + "/self-register";

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
        ESP_LOGW(TAG, "Re-register: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(json_str);
        return;
    }

    esp_http_client_write(client, json_str, json_len);
    free(json_str);

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status == 200 && content_len > 0 && content_len < 1024) {
        char* resp = (char*)malloc(content_len + 1);
        if (resp) {
            int read_len = esp_http_client_read(client, resp, content_len);
            if (read_len > 0) {
                resp[read_len] = '\0';
                cJSON* root = cJSON_Parse(resp);
                if (root) {
                    cJSON* data = cJSON_GetObjectItem(root, "data");
                    if (data) {
                        cJSON* token_item = cJSON_GetObjectItem(data, "device_token");
                        if (token_item && token_item->valuestring) {
                            device_token_ = token_item->valuestring;
                            // Save to NVS
                            Settings settings("xiaolu", true);
                            settings.SetString("device_token", token_item->valuestring);
                            ESP_LOGI(TAG, "Re-register SUCCESS! New token saved.");
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            free(resp);
        }
    } else if (status == 403) {
        // 设备已被用户解绑。只擦 token，保留 WiFi 连接。
        // 不重启、不擦 WiFi — 保持联网状态等待用户在 App 重新绑定后自注册成功。
        ESP_LOGW(TAG, "Re-register REJECTED (403): device unbound. Clearing token, will retry later.");

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        {
            Settings settings("xiaolu", true);
            settings.EraseKey("device_token");
            settings.EraseKey("child_id");
        }
        device_token_.clear();

        // 等 60 秒后再重试，避免疯狂请求服务器
        for (int i = 0; i < 120 && running_; i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        return;
    } else {
        ESP_LOGW(TAG, "Re-register failed: status=%d", status);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

// ───────── Command de-duplication NVS helpers ─────────

void XiaoLuHeartbeat::LoadLastProcessedId() {
    Settings s("xiaolu", false);
    last_processed_id_ = s.GetInt("last_cmd_id", 0);
}

void XiaoLuHeartbeat::SaveLastProcessedId(int id) {
    if (id <= last_processed_id_) return;
    last_processed_id_ = id;
    Settings s("xiaolu", true);
    s.SetInt("last_cmd_id", id);
}
