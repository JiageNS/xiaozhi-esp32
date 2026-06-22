#ifndef XIAOLU_HEARTBEAT_H
#define XIAOLU_HEARTBEAT_H

#include <string>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct DeviceCommand {
    int id;
    std::string command;
    std::string params;  // JSON string
};

/**
 * XiaoLuHeartbeat — 小鹿豆心跳服务（HTTP 轮询）
 *
 * 每 3 秒向服务器发送心跳，同时接收命令。
 * 命令延迟 < 3 秒。
 */
class XiaoLuHeartbeat {
public:
    using CommandCallback = std::function<void(const DeviceCommand&)>;

    XiaoLuHeartbeat();
    ~XiaoLuHeartbeat();

    void SetServerConfig(const std::string& server_url, const std::string& device_token);
    void SetDeviceId(const std::string& id) { device_id_ = id; }
    void SetCommandCallback(CommandCallback cb) { command_callback_ = cb; }
    void Start(int interval_sec = 3);
    void Stop();

    void SetBattery(int percent) { battery_ = percent; }
    void SetRecording(bool recording) { is_recording_ = recording; }
    void SetWifiRssi(int rssi) { wifi_rssi_ = rssi; }
    void SetStorageUsed(int mb) { storage_used_mb_ = mb; }
    void SetStorageTotal(int mb) { storage_total_mb_ = mb; }
    void SetFirmwareVersion(const std::string& ver) { firmware_version_ = ver; }

    // 上传进度（由 LocalStorageProtocol 更新）
    void SetUploadStatus(bool uploading, int current, int total, int percent) {
        upload_active_ = uploading;
        upload_current_ = current;
        upload_total_ = total;
        upload_percent_ = percent;
    }

private:
    std::string server_url_;
    std::string device_token_;
    std::string device_id_;
    CommandCallback command_callback_;

    int battery_ = 100;
    bool is_recording_ = false;
    int wifi_rssi_ = 0;
    int storage_used_mb_ = 0;
    int storage_total_mb_ = 8192;
    std::string firmware_version_ = "1.0.0";

    // 上传进度
    bool upload_active_ = false;
    int upload_current_ = 0;
    int upload_total_ = 0;
    int upload_percent_ = 0;

    void* timer_ = nullptr;
    bool running_ = false;
    TaskHandle_t task_handle_ = nullptr;

    void HeartbeatTask();
    void DoHeartbeat();
    void DoReRegister();
    void ParseResponse(const char* json, int len);

    // Command de-duplication
    int last_processed_id_ = 0;     // monotonically increasing; loaded from NVS
    std::vector<int> pending_acks_;  // ids handled this cycle, sent up next heartbeat
    void LoadLastProcessedId();
    void SaveLastProcessedId(int id);

    static void TaskEntry(void* arg);
};

#endif // XIAOLU_HEARTBEAT_H
