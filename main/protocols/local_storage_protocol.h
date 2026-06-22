#ifndef LOCAL_STORAGE_PROTOCOL_H
#define LOCAL_STORAGE_PROTOCOL_H

#include "protocol.h"
#include <string>
#include <cstdio>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/**
 * LocalStorageProtocol — 小鹿豆录音协议
 *
 * 两种模式：
 * 1. WiFi 手动上传模式（无 SD 卡）：
 *    音频帧缓存在内存，持续录音不自动上传；
 *    用户点"立即上传"时通过 FlushAndUpload() 触发上传；
 *    仅在缓冲区快满时才被迫自动上传（避免数据丢失）
 * 2. SD 卡本地存储模式（有 SD 卡，手搓版默认）：
 *    音频帧持续写入 SD 卡单个文件（不切片，可录数小时）；
 *    不自动上传，用户点"立即上传"时关闭文件并整个上传；
 *    服务端收到后按会话切分分析
 *
 * 当前手搓版默认使用 SD 卡模式。
 */

struct RecordingFileInfo {
    std::string path;
    int64_t start_time_ms = 0;
    int duration_sec = 0;
    int file_size = 0;
};

class LocalStorageProtocol : public Protocol {
public:
    LocalStorageProtocol(const std::string& storage_path);
    ~LocalStorageProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    int GetPendingFileCount() const { return pending_file_count_; }
    std::vector<RecordingFileInfo> GetPendingFiles() const;
    void MarkFileUploaded(const std::string& path);

    /** 设置服务器配置（WiFi 上传模式用） */
    void SetServerConfig(const std::string& url, const std::string& token);

    /** 立即刷新缓冲区并上传 */
    void FlushAndUpload();

    /** 电量更新（低电量时触发紧急落盘） */
    void OnBatteryUpdate(int percent, bool charging);

    /** 强制把当前 SD 卡切片完整落盘并关闭（用于低电量 / 关机前 flush）*/
    void ForceFlushToSd();

    /** 获取 SD 卡存储信息（MB） */
    bool HasSdCard() const { return has_sd_card_; }
    int GetStorageUsedMb() const { return storage_used_mb_; }
    int GetStorageTotalMb() const { return storage_total_mb_; }
    void UpdateStorageInfo();

    // 上传进度（供 heartbeat 读取）
    bool IsUploading() const { return scan_running_; }
    int GetUploadCurrentFile() const { return upload_file_index_; }
    int GetUploadTotalFiles() const { return upload_file_total_; }
    int GetUploadFilePercent() const { return upload_file_percent_; }

protected:
    bool SendText(const std::string& text) override;

private:
    std::string storage_path_;
    std::string server_url_;
    std::string device_token_;

    // WiFi 上传模式：内存缓冲区
    uint8_t* audio_buffer_ = nullptr;
    int buffer_size_ = 0;
    int buffer_capacity_ = 0;
    int frame_count_ = 0;
    int slice_index_ = 0;
    int64_t slice_start_time_ = 0;
    SemaphoreHandle_t buffer_mutex_ = nullptr;
    SemaphoreHandle_t file_mutex_ = nullptr;  // 保护 SD 卡文件操作
    TaskHandle_t upload_task_ = nullptr;
    bool upload_pending_ = false;

    // SD 卡模式
    FILE* current_file_ = nullptr;
    std::string current_file_path_;       // 当前 .xlop 文件路径
    std::string current_file_meta_path_;  // 对应 .meta 文件路径（sidecar）
    std::string current_hour_bucket_;     // 当前小时文件夹名 YYYYMMDD_HH
    int64_t slice_start_wall_ms_ = 0;     // 当前切片录音开始的墙钟时间
    int64_t file_start_time_ = 0;
    int total_recorded_sec_ = 0;
    int pending_file_count_ = 0;

    // 低电量保护
    int last_battery_percent_ = 100;
    bool is_charging_ = false;
    bool emergency_flushed_ = false;  // 本次低电量已紧急落盘过，避免重复触发
    int frames_since_flush_ = 0;      // 距离上次 fsync 多少帧，用于周期性落盘

    bool channel_opened_ = false;
    bool has_sd_card_ = false;
    bool started_ = false;  // 幂等保护：避免重复 Start() 启动多个扫描任务
    int storage_used_mb_ = 0;
    int storage_total_mb_ = 0;
    bool wifi_connected_ = false;

    static constexpr int kSliceDurationSec = 300;   // 5 分钟切片
    static constexpr int kFrameDurationMs = 60;
    // 5 分钟 Opus 音频约 300KB（16kbps * 300s / 8 = 600KB max, 实际约 200-400KB）
    static constexpr int kBufferCapacity = 512 * 1024; // 512KB 缓冲区

    void StartNewSlice();
    void FlushSlice();
    bool UploadBuffer(const uint8_t* data, int len, int duration);
    bool UploadBufferWithTime(const uint8_t* data, int len, int duration, int64_t start_wall_ms);
    static void UploadTaskFunc(void* arg);

    // SD 卡模式
    void StartNewFile();
    void CloseCurrentFile();
    void CloseCurrentFileUnlocked();
    std::string BuildHourDir(int64_t wall_ms) const;
    std::string BuildFilePath(int64_t wall_ms, std::string& meta_out) const;
    void WriteMeta(const std::string& meta_path, int64_t start_wall_ms, int duration_sec, int frame_count, int file_size);
    bool ReadMeta(const std::string& meta_path, int64_t& start_wall_ms, int& duration_sec);
    void ScanExistingFiles();
    void CleanupOldFiles();
    bool CheckSdCard();

    // 后台扫描上传（SD 卡模式，保留但不再自动启动）
    TaskHandle_t scan_task_ = nullptr;
    bool scan_running_ = false;
    bool upload_again_ = false;  // 上传任务运行中收到新的上传请求时置 true
    std::string current_upload_session_;  // 当前上传批次 ID
    int upload_file_index_ = 0;   // 当前正在上传第几个文件（1-based）
    int upload_file_total_ = 0;   // 本批次总文件数
    int upload_file_percent_ = 0; // 当前文件上传进度百分比
    static void ScanUploadTaskFunc(void* arg);
    static void OneShotUploadTaskFunc(void* arg);
    bool UploadOneSdFile(const std::string& xlop_path, const std::string& meta_path);
    bool NotifyUploadComplete(const std::string& upload_session_id);
};

#endif
