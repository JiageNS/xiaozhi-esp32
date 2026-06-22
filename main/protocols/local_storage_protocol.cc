#include "local_storage_protocol.h"
#include "xiaolu_time.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_vfs_fat.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <algorithm>

#define TAG "LocalStorage"

LocalStorageProtocol::LocalStorageProtocol(const std::string& storage_path)
    : storage_path_(storage_path) {
    buffer_mutex_ = xSemaphoreCreateMutex();
    file_mutex_ = xSemaphoreCreateMutex();
}

LocalStorageProtocol::~LocalStorageProtocol() {
    scan_running_ = false;
    CloseAudioChannel();
    if (audio_buffer_) { free(audio_buffer_); audio_buffer_ = nullptr; }
    if (buffer_mutex_) vSemaphoreDelete(buffer_mutex_);
    if (file_mutex_) vSemaphoreDelete(file_mutex_);
}

void LocalStorageProtocol::SetServerConfig(const std::string& url, const std::string& token) {
    server_url_ = url;
    device_token_ = token;
    ESP_LOGI(TAG, "Server config: %s", server_url_.c_str());
}

bool LocalStorageProtocol::Start() {
    // 幂等保护：避免被重复调用时启动多个扫描任务，导致同一文件被并发上传多次。
    if (started_) {
        ESP_LOGW(TAG, "Start() already called, ignoring duplicate");
        return true;
    }
    started_ = true;

    // 检查是否有 SD 卡
    has_sd_card_ = CheckSdCard();

    if (has_sd_card_) {
        // SD 卡模式：统一用 /sdcard/recordings 作为根目录（忽略构造时传入的 storage_path_）
        storage_path_ = "/sdcard/recordings";
        struct stat st;
        if (stat(storage_path_.c_str(), &st) != 0) {
            mkdir(storage_path_.c_str(), 0755);
        }
        ScanExistingFiles();
        ESP_LOGI(TAG, "SD card mode. Root: %s, pending: %d", storage_path_.c_str(), pending_file_count_);

        // 不自动启动后台上传，等用户点"立即上传"时通过 FlushAndUpload() 触发
    } else {
        // WiFi 实时上传模式：分配内存缓冲区
        audio_buffer_ = (uint8_t*)malloc(kBufferCapacity);
        if (!audio_buffer_) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for audio buffer!", kBufferCapacity);
            // 尝试更小的缓冲区
            audio_buffer_ = (uint8_t*)malloc(256 * 1024);
            if (audio_buffer_) {
                ESP_LOGW(TAG, "Using reduced 256KB buffer");
            } else {
                ESP_LOGE(TAG, "Cannot allocate audio buffer, recording disabled");
                return false;
            }
        }
        buffer_size_ = 0;
        buffer_capacity_ = audio_buffer_ ? kBufferCapacity : 0;
        ESP_LOGI(TAG, "WiFi stream mode. Buffer: %d KB", buffer_capacity_ / 1024);
    }

    if (on_connected_) on_connected_();
    return true;
}

bool LocalStorageProtocol::OpenAudioChannel() {
    if (!channel_opened_) {
        if (has_sd_card_) {
            StartNewFile();
        } else {
            StartNewSlice();
        }
        channel_opened_ = true;
        if (on_audio_channel_opened_) on_audio_channel_opened_();
        ESP_LOGI(TAG, "Audio channel opened (%s)", has_sd_card_ ? "SD card" : "WiFi stream");
    }
    return true;
}

void LocalStorageProtocol::CloseAudioChannel(bool send_goodbye) {
    if (channel_opened_) {
        // 先标记关闭，阻止新的 SendAudio 写入
        channel_opened_ = false;
        // 拿到 file_mutex_ 确保 SendAudio 已经完成当前帧写入
        if (has_sd_card_) {
            xSemaphoreTake(file_mutex_, portMAX_DELAY);
            CloseCurrentFileUnlocked();
            xSemaphoreGive(file_mutex_);
        } else {
            FlushSlice();
        }
        if (on_audio_channel_closed_) on_audio_channel_closed_();
        ESP_LOGI(TAG, "Audio channel closed");
    }
}

bool LocalStorageProtocol::IsAudioChannelOpened() const {
    return channel_opened_;
}

bool LocalStorageProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!channel_opened_ || !packet) return false;
    const auto& data = packet->payload;
    if (data.empty()) return true;

    if (has_sd_card_) {
        // SD 卡模式：写文件
        xSemaphoreTake(file_mutex_, portMAX_DELAY);
        if (!channel_opened_) {
            // 在等 mutex 期间通道被关闭了
            xSemaphoreGive(file_mutex_);
            return false;
        }
        // 如果还没有当前文件（可能是因为之前时间未同步），尝试开一个
        if (!current_file_ && XiaoLuTime::IsSynced()) {
            StartNewFile();
        }
        if (current_file_) {
            uint16_t frame_len = (uint16_t)data.size();
            fwrite(&frame_len, 2, 1, current_file_);
            fwrite(data.data(), 1, data.size(), current_file_);
            frame_count_++;
            frames_since_flush_++;

            // 每 250 帧（15 秒）做一次 fflush + fsync，保证断电最多丢 15 秒
            static constexpr int kFlushEveryFrames = 250;
            if (frames_since_flush_ >= kFlushEveryFrames) {
                fflush(current_file_);
                int fd = fileno(current_file_);
                if (fd >= 0) fsync(fd);
                frames_since_flush_ = 0;
            }

            // 每 5 分钟自动切片，避免产生巨型文件导致上传超时
            static constexpr int kMaxFramesPerFile = 5000; // 5min @ 60ms/frame
            if (frame_count_ >= kMaxFramesPerFile) {
                CloseCurrentFileUnlocked();
                StartNewFile();
            }
        }
        xSemaphoreGive(file_mutex_);
        // 时间未同步时整帧丢弃，录音从时间同步后才开始保存
    } else {
        // WiFi 实时上传模式：写入内存缓冲区
        xSemaphoreTake(buffer_mutex_, portMAX_DELAY);

        int needed = 2 + data.size(); // frame_len + data
        if (buffer_size_ + needed <= buffer_capacity_) {
            uint16_t frame_len = (uint16_t)data.size();
            memcpy(audio_buffer_ + buffer_size_, &frame_len, 2);
            buffer_size_ += 2;
            memcpy(audio_buffer_ + buffer_size_, data.data(), data.size());
            buffer_size_ += data.size();
            frame_count_++;
        }
        // 如果缓冲区满了，丢弃新数据（等待上传完成）

        // 不自动按时间上传，只在缓冲区快满时被迫上传（避免数据丢失）
        // 用户点"立即上传"按钮时通过 FlushAndUpload() 手动触发
        bool need_flush = (buffer_size_ + 200 >= buffer_capacity_);

        xSemaphoreGive(buffer_mutex_);

        if (need_flush && !upload_pending_) {
            upload_pending_ = true;
            if (!upload_task_) {
                xTaskCreate(UploadTaskFunc, "xl_upload", 8192, this, 3, &upload_task_);
            }
        }
    }

    return true;
}

bool LocalStorageProtocol::SendText(const std::string& text) {
    return true;
}

// ─── WiFi 实时上传模式 ───

void LocalStorageProtocol::StartNewSlice() {
    xSemaphoreTake(buffer_mutex_, portMAX_DELAY);
    buffer_size_ = 0;
    frame_count_ = 0;
    slice_start_time_ = esp_timer_get_time() / 1000;
    slice_start_wall_ms_ = XiaoLuTime::IsSynced() ? XiaoLuTime::WallNowMs() : 0;
    xSemaphoreGive(buffer_mutex_);
    ESP_LOGI(TAG, "New slice #%d started", slice_index_);
}

void LocalStorageProtocol::FlushSlice() {
    xSemaphoreTake(buffer_mutex_, portMAX_DELAY);
    int size = buffer_size_;
    int duration = (frame_count_ * kFrameDurationMs) / 1000;
    int64_t start_wall = slice_start_wall_ms_;
    xSemaphoreGive(buffer_mutex_);

    if (size == 0) {
        ESP_LOGI(TAG, "Slice #%d empty, skipping upload", slice_index_);
        return;
    }

    ESP_LOGI(TAG, "Flushing slice #%d: %d bytes, %ds", slice_index_, size, duration);

    if (UploadBufferWithTime(audio_buffer_, size, duration, start_wall)) {
        ESP_LOGI(TAG, "Slice #%d uploaded successfully", slice_index_);
        slice_index_++;
    } else {
        ESP_LOGW(TAG, "Slice #%d upload failed, data lost", slice_index_);
        slice_index_++;
    }
}

bool LocalStorageProtocol::UploadBuffer(const uint8_t* data, int len, int duration) {
    int64_t start_wall = XiaoLuTime::IsSynced()
        ? (XiaoLuTime::WallNowMs() - (int64_t)duration * 1000)
        : 0;
    return UploadBufferWithTime(data, len, duration, start_wall);
}

bool LocalStorageProtocol::UploadBufferWithTime(const uint8_t* data, int len, int duration, int64_t start_wall_ms) {
    // Always read the freshest token from NVS — heartbeat may have refreshed it
    // after a 401, but our cached device_token_ would still hold the stale one.
    std::string token;
    {
        Settings s("xiaolu", false);
        token = s.GetString("device_token");
    }
    if (token.empty()) token = device_token_;  // fallback

    if (server_url_.empty() || token.empty()) {
        ESP_LOGW(TAG, "No server config, cannot upload");
        return false;
    }

    std::string url = server_url_ + "/stream/upload";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 60000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 4096;
    config.skip_cert_common_name_check = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // 设置请求头
    std::string auth = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    char dur_str[16], idx_str[16];
    snprintf(dur_str, sizeof(dur_str), "%d", duration);
    snprintf(idx_str, sizeof(idx_str), "%d", slice_index_);
    esp_http_client_set_header(client, "X-Audio-Duration", dur_str);
    esp_http_client_set_header(client, "X-Audio-Format", "opus");
    esp_http_client_set_header(client, "X-Slice-Index", idx_str);

    // 录音开始的真实墙钟时间（北京时区 ISO）
    if (start_wall_ms > 0) {
        std::string iso = XiaoLuTime::FormatIso(start_wall_ms);
        esp_http_client_set_header(client, "X-Recording-Start-Time", iso.c_str());
    }

    // 发送数据
    esp_err_t err = esp_http_client_open(client, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    // 分块写入
    int total_written = 0;
    const int CHUNK = 4096;
    while (total_written < len) {
        int to_write = (len - total_written) > CHUNK ? CHUNK : (len - total_written);
        int written = esp_http_client_write(client, (const char*)(data + total_written), to_write);
        if (written < 0) {
            ESP_LOGE(TAG, "HTTP write failed at %d/%d", total_written, len);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        total_written += written;
    }

    // 读取响应
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    bool success = (status >= 200 && status < 300);
    if (!success) {
        ESP_LOGW(TAG, "Upload returned HTTP %d", status);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

void LocalStorageProtocol::UploadTaskFunc(void* arg) {
    auto* self = static_cast<LocalStorageProtocol*>(arg);
    ESP_LOGI(TAG, "Upload task started");

    // 拷贝缓冲区数据（避免长时间持锁）
    xSemaphoreTake(self->buffer_mutex_, portMAX_DELAY);
    int size = self->buffer_size_;
    int duration = (self->frame_count_ * kFrameDurationMs) / 1000;
    int64_t start_wall = self->slice_start_wall_ms_;
    // 分配临时缓冲区拷贝数据
    uint8_t* upload_buf = (uint8_t*)malloc(size);
    if (upload_buf) {
        memcpy(upload_buf, self->audio_buffer_, size);
    }
    // 清空主缓冲区，让录音继续
    self->buffer_size_ = 0;
    self->frame_count_ = 0;
    self->slice_start_wall_ms_ = XiaoLuTime::IsSynced() ? XiaoLuTime::WallNowMs() : 0;
    xSemaphoreGive(self->buffer_mutex_);

    if (upload_buf && size > 0) {
        ESP_LOGI(TAG, "Uploading slice #%d: %d bytes, %ds", self->slice_index_, size, duration);
        if (self->UploadBufferWithTime(upload_buf, size, duration, start_wall)) {
            ESP_LOGI(TAG, "Slice #%d uploaded OK", self->slice_index_);
        } else {
            ESP_LOGW(TAG, "Slice #%d upload FAILED", self->slice_index_);
        }
        self->slice_index_++;
        free(upload_buf);
    } else {
        if (upload_buf) free(upload_buf);
        ESP_LOGW(TAG, "Upload: no data or alloc failed");
    }

    self->upload_pending_ = false;
    self->upload_task_ = nullptr;
    vTaskDelete(nullptr);
}

// ─── SD 卡模式 ───

bool LocalStorageProtocol::CheckSdCard() {
    struct stat st;
    // 检查 /sdcard 是否可用
    if (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode)) {
        ESP_LOGI(TAG, "SD card detected at /sdcard");
        return true;
    }
    ESP_LOGI(TAG, "No SD card detected, using WiFi stream mode");
    return false;
}

void LocalStorageProtocol::UpdateStorageInfo() {
    if (!has_sd_card_) {
        storage_used_mb_ = 0;
        storage_total_mb_ = 0;
        return;
    }
    // Use FATFS API to get SD card space
    FATFS* fs;
    DWORD free_clusters;
    // Try common drive numbers for SDMMC
    const char* drives[] = {"0:", "1:", "2:"};
    for (int i = 0; i < 3; i++) {
        if (f_getfree(drives[i], &free_clusters, &fs) == FR_OK && fs->n_fatent > 2) {
            uint64_t total_sectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
            uint64_t free_sectors = (uint64_t)free_clusters * fs->csize;
            uint64_t sector_size = fs->ssize > 0 ? fs->ssize : 512;
            storage_total_mb_ = (int)((total_sectors * sector_size) / (1024 * 1024));
            storage_used_mb_ = (int)(((total_sectors - free_sectors) * sector_size) / (1024 * 1024));
            return;
        }
    }
}

void LocalStorageProtocol::StartNewFile() {
    if (!XiaoLuTime::IsSynced()) {
        ESP_LOGW(TAG, "Time not synced yet, defer SD file creation");
        return;
    }
    int64_t now_ms = XiaoLuTime::WallNowMs();

    // 确保录音根目录存在（FAT32 不支持嵌套 mkdir，改用扁平结构）
    struct stat st;
    if (stat(storage_path_.c_str(), &st) != 0) {
        if (mkdir(storage_path_.c_str(), 0) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "Cannot create %s (errno=%d), using /sdcard directly", storage_path_.c_str(), errno);
            storage_path_ = "/sdcard";
        }
    }
    current_hour_bucket_ = XiaoLuTime::FormatHourBucket(now_ms);
    // Use flat file naming: /sdcard/recordings/20260514_170802.xlop (no subdirectory)
    std::string hms = XiaoLuTime::FormatHms(now_ms);
    std::string date_hour = XiaoLuTime::FormatHourBucket(now_ms);
    current_file_path_ = storage_path_ + "/" + date_hour + "_" + hms + ".xlop";
    current_file_meta_path_ = storage_path_ + "/" + date_hour + "_" + hms + ".meta";

    current_file_ = fopen(current_file_path_.c_str(), "wb");
    if (!current_file_) {
        ESP_LOGE(TAG, "Failed to create: %s (errno=%d)", current_file_path_.c_str(), errno);
        return;
    }
    const char magic[] = "XLOP";
    uint32_t sample_rate = 16000;
    uint8_t channels = 1;
    uint8_t frame_ms = kFrameDurationMs;
    fwrite(magic, 4, 1, current_file_);
    fwrite(&sample_rate, 4, 1, current_file_);
    fwrite(&channels, 1, 1, current_file_);
    fwrite(&frame_ms, 1, 1, current_file_);
    file_start_time_ = esp_timer_get_time() / 1000;
    slice_start_wall_ms_ = now_ms;
    frame_count_ = 0;
    frames_since_flush_ = 0;
    ESP_LOGI(TAG, "New file: %s", current_file_path_.c_str());
}

void LocalStorageProtocol::CloseCurrentFile() {
    xSemaphoreTake(file_mutex_, portMAX_DELAY);
    CloseCurrentFileUnlocked();
    xSemaphoreGive(file_mutex_);
}

void LocalStorageProtocol::CloseCurrentFileUnlocked() {
    if (current_file_) {
        int file_size = (int)ftell(current_file_);
        fclose(current_file_);
        current_file_ = nullptr;
        int duration = (frame_count_ * kFrameDurationMs) / 1000;
        total_recorded_sec_ += duration;
        pending_file_count_++;
        WriteMeta(current_file_meta_path_, slice_start_wall_ms_, duration, frame_count_, file_size);
        ESP_LOGI(TAG, "Closed: %s (%ds, %d frames, %d bytes)",
                 current_file_path_.c_str(), duration, frame_count_, file_size);
    }
}

std::string LocalStorageProtocol::BuildHourDir(int64_t wall_ms) const {
    return storage_path_ + "/" + XiaoLuTime::FormatHourBucket(wall_ms);
}

std::string LocalStorageProtocol::BuildFilePath(int64_t wall_ms, std::string& meta_out) const {
    std::string hour_dir = BuildHourDir(wall_ms);
    std::string hms = XiaoLuTime::FormatHms(wall_ms);
    std::string base = hour_dir + "/s_" + hms;
    meta_out = base + ".meta";
    return base + ".xlop";
}

void LocalStorageProtocol::WriteMeta(const std::string& meta_path, int64_t start_wall_ms,
                                     int duration_sec, int frame_count, int file_size) {
    std::string iso = XiaoLuTime::FormatIso(start_wall_ms);
    std::string ms_str = XiaoLuTime::FormatI64(start_wall_ms);
    char buf[256];
    // 注意：CONFIG_NEWLIB_NANO_FORMAT=y 下 newlib-nano 不支持 %lld/%llu，
    // 用 %lld 会让后续 %s 读取错位的指针导致崩溃。这里把 int64_t 先转成字符串再用 %s 输出。
    int len = snprintf(buf, sizeof(buf),
        "{\"start_wall_ms\":%s,\"start_iso\":\"%s\",\"duration_sec\":%d,\"frame_count\":%d,\"file_size\":%d,\"uploaded\":false}\n",
        ms_str.c_str(), iso.c_str(), duration_sec, frame_count, file_size);
    int fd = open(meta_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) return;
    write(fd, buf, len);
    close(fd);
}

bool LocalStorageProtocol::ReadMeta(const std::string& meta_path, int64_t& start_wall_ms, int& duration_sec) {
    FILE* f = fopen(meta_path.c_str(), "r");
    if (!f) return false;
    char buf[256] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;

    // 极简解析：找 "start_wall_ms":NUM 和 "duration_sec":NUM
    // 注意：CONFIG_NEWLIB_NANO_FORMAT=y 下 nano-scanf 不支持 %lld，需要手动解析数字
    const char* p;
    start_wall_ms = 0;
    duration_sec = 0;
    if ((p = strstr(buf, "\"start_wall_ms\":")) != nullptr) {
        const char* q = p + 16;
        while (*q == ' ') q++;
        bool neg = false;
        if (*q == '-') { neg = true; q++; }
        int64_t v = 0;
        while (*q >= '0' && *q <= '9') {
            v = v * 10 + (*q - '0');
            q++;
        }
        start_wall_ms = neg ? -v : v;
    }
    if ((p = strstr(buf, "\"duration_sec\":")) != nullptr) {
        int v = 0;
        if (sscanf(p + 15, "%d", &v) == 1) duration_sec = v;
    }
    return start_wall_ms > 0;
}

std::vector<RecordingFileInfo> LocalStorageProtocol::GetPendingFiles() const {
    std::vector<RecordingFileInfo> files;
    if (!has_sd_card_) return files;
    DIR* root = opendir(storage_path_.c_str());
    if (!root) return files;
    struct dirent* entry;
    while ((entry = readdir(root)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string path = storage_path_ + "/" + entry->d_name;
        // Check if it's a .xlop file directly in storage_path_ (flat structure)
        if (strstr(entry->d_name, ".xlop")) {
            if (path == current_file_path_) continue;  // Skip currently recording file
            RecordingFileInfo info;
            info.path = path;
            struct stat fs;
            if (stat(info.path.c_str(), &fs) == 0) {
                info.file_size = fs.st_size;
            }
            files.push_back(info);
            continue;
        }
        // Also check subdirectories (legacy structure)
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        DIR* dir = opendir(path.c_str());
        if (!dir) continue;
        struct dirent* e2;
        while ((e2 = readdir(dir)) != nullptr) {
            if (!strstr(e2->d_name, ".xlop")) continue;
            RecordingFileInfo info;
            info.path = path + "/" + e2->d_name;
            if (info.path == current_file_path_) continue;
            struct stat fs2;
            if (stat(info.path.c_str(), &fs2) == 0) {
                info.file_size = fs2.st_size;
            }
            files.push_back(info);
        }
        closedir(dir);
    }
    closedir(root);
    return files;
}

void LocalStorageProtocol::MarkFileUploaded(const std::string& path) {
    if (remove(path.c_str()) == 0) {
        // 同时删 .meta
        std::string meta = path;
        size_t dot = meta.rfind(".xlop");
        if (dot != std::string::npos) {
            meta.replace(dot, 5, ".meta");
            remove(meta.c_str());
        }
        if (pending_file_count_ > 0) pending_file_count_--;
        ESP_LOGI(TAG, "Deleted: %s", path.c_str());
    }
}

void LocalStorageProtocol::ScanExistingFiles() {
    pending_file_count_ = 0;
    DIR* root = opendir(storage_path_.c_str());
    if (!root) return;
    struct dirent* entry;
    while ((entry = readdir(root)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string sub = storage_path_ + "/" + entry->d_name;
        struct stat st;
        if (stat(sub.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        DIR* dir = opendir(sub.c_str());
        if (!dir) continue;
        struct dirent* e2;
        while ((e2 = readdir(dir)) != nullptr) {
            if (strstr(e2->d_name, ".xlop")) pending_file_count_++;
        }
        closedir(dir);
    }
    closedir(root);
}

void LocalStorageProtocol::CleanupOldFiles() {
    int deleted = 0;
    DIR* root = opendir(storage_path_.c_str());
    if (!root) return;
    struct dirent* entry;
    std::vector<std::string> to_delete;
    while ((entry = readdir(root)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string path = storage_path_ + "/" + entry->d_name;
        if (strstr(entry->d_name, ".xlop") || strstr(entry->d_name, ".meta")) {
            to_delete.push_back(path);
        }
        // 也清理旧的子目录结构
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR* dir = opendir(path.c_str());
            if (dir) {
                struct dirent* e2;
                while ((e2 = readdir(dir)) != nullptr) {
                    if (e2->d_name[0] == '.') continue;
                    to_delete.push_back(path + "/" + e2->d_name);
                }
                closedir(dir);
                to_delete.push_back(path);  // 删空目录
            }
        }
    }
    closedir(root);

    for (const auto& f : to_delete) {
        remove(f.c_str());
        deleted++;
    }
    if (deleted > 0) {
        ESP_LOGI(TAG, "Cleaned up %d old files from SD card", deleted);
    }
    pending_file_count_ = 0;
}

void LocalStorageProtocol::FlushAndUpload() {
    if (has_sd_card_) {
        // SD 卡模式：关闭当前录音文件，启动一次性上传任务
        xSemaphoreTake(file_mutex_, portMAX_DELAY);
        CloseCurrentFileUnlocked();
        xSemaphoreGive(file_mutex_);

        if (!scan_running_) {
            scan_running_ = true;
            xTaskCreate(OneShotUploadTaskFunc, "xl_upload_once", 8192, this, 3, &scan_task_);
        } else {
            // 上传任务正在跑，标记让它完成后再扫一轮
            upload_again_ = true;
            ESP_LOGI(TAG, "Upload already running, will re-scan after current batch");
        }
    } else {
        // WiFi 内存模式
        if (buffer_size_ > 0 && !upload_pending_) {
            upload_pending_ = true;
            if (!upload_task_) {
                xTaskCreate(UploadTaskFunc, "xl_upload", 8192, this, 3, &upload_task_);
            }
        }
    }
}

// ─── 后台扫描上传（SD 卡模式）───
// 每 60 秒扫一次 /sdcard/recordings/，有 WiFi 就挨个上传 .xlop 切片

void LocalStorageProtocol::ScanUploadTaskFunc(void* arg) {
    auto* self = static_cast<LocalStorageProtocol*>(arg);
    ESP_LOGI(TAG, "SD scan upload task started");

    // 等 WiFi 稳定
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (self->scan_running_) {
        // 列出所有 pending .xlop 文件
        std::vector<RecordingFileInfo> files = self->GetPendingFiles();

        if (!files.empty()) {
            ESP_LOGI(TAG, "SD scan: %d pending files to upload", (int)files.size());

            // 按路径排序（文件夹名 + 文件名都包含时间戳，天然有序）
            std::sort(files.begin(), files.end(),
                      [](const RecordingFileInfo& a, const RecordingFileInfo& b) {
                          return a.path < b.path;
                      });

            for (const auto& f : files) {
                if (!self->scan_running_) break;
                // 对应的 meta
                std::string meta = f.path;
                size_t dot = meta.rfind(".xlop");
                if (dot != std::string::npos) meta.replace(dot, 5, ".meta");

                bool ok = self->UploadOneSdFile(f.path, meta);
                if (ok) {
                    self->MarkFileUploaded(f.path);
                } else {
                    ESP_LOGW(TAG, "SD upload failed: %s (will retry next scan)", f.path.c_str());
                    // 上传失败：停止本轮，等下一次扫描再试（避免 WiFi 抖动时狂刷）
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));  // 每个文件之间喘口气
            }
        }

        // 每 60 秒扫一次
        for (int i = 0; i < 60 && self->scan_running_; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "SD scan upload task stopped");
    self->scan_task_ = nullptr;
    vTaskDelete(nullptr);
}

void LocalStorageProtocol::OneShotUploadTaskFunc(void* arg) {
    auto* self = static_cast<LocalStorageProtocol*>(arg);
    ESP_LOGI(TAG, "One-shot upload triggered by user");

    // 生成本次上传批次的 session_id
    char session_id[48];
    snprintf(session_id, sizeof(session_id), "batch_%ld", (long)(esp_timer_get_time() / 1000));
    self->current_upload_session_ = session_id;
    int uploaded_count = 0;

    do {
        self->upload_again_ = false;

        std::vector<RecordingFileInfo> files = self->GetPendingFiles();
        if (!files.empty()) {
            std::sort(files.begin(), files.end(),
                      [](const RecordingFileInfo& a, const RecordingFileInfo& b) {
                          return a.path < b.path;
                      });

            ESP_LOGI(TAG, "Uploading %d pending files (session=%s)", (int)files.size(), session_id);
            self->upload_file_total_ = (int)files.size();
            self->upload_file_index_ = 0;
            for (const auto& f : files) {
                self->upload_file_index_++;
                self->upload_file_percent_ = 0;
                std::string meta = f.path;
                size_t dot = meta.rfind(".xlop");
                if (dot != std::string::npos) meta.replace(dot, 5, ".meta");

                if (self->UploadOneSdFile(f.path, meta)) {
                    self->MarkFileUploaded(f.path);
                    if (f.file_size <= 10 * 1024 * 1024) {
                        uploaded_count++;
                    }
                } else {
                    ESP_LOGW(TAG, "Upload failed: %s, stopping", f.path.c_str());
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        } else {
            ESP_LOGI(TAG, "No pending files to upload");
        }
    } while (self->upload_again_);

    // 重置上传进度
    self->upload_file_index_ = 0;
    self->upload_file_total_ = 0;
    self->upload_file_percent_ = 0;

    // 所有切片上传完成后，通知服务器拼接并分析
    if (uploaded_count > 0) {
        ESP_LOGI(TAG, "All %d slices uploaded, notifying server to merge (session=%s)", uploaded_count, session_id);
        if (!self->NotifyUploadComplete(std::string(session_id))) {
            ESP_LOGW(TAG, "upload-complete notification failed");
        }
    }

    // 上传完毕后重新开始录音到新文件
    xSemaphoreTake(self->file_mutex_, portMAX_DELAY);
    if (self->channel_opened_ && !self->current_file_) {
        self->StartNewFile();
    }
    xSemaphoreGive(self->file_mutex_);

    self->scan_running_ = false;
    self->scan_task_ = nullptr;
    vTaskDelete(nullptr);
}

bool LocalStorageProtocol::UploadOneSdFile(const std::string& xlop_path, const std::string& meta_path) {
    // 读 meta 拿真实起始时间
    int64_t start_wall_ms = 0;
    int duration_sec = 0;
    ReadMeta(meta_path, start_wall_ms, duration_sec);

    FILE* f = fopen(xlop_path.c_str(), "rb");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s", xlop_path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 10) {
        fclose(f);
        ESP_LOGW(TAG, "File too small: %s (%ld bytes)", xlop_path.c_str(), fsize);
        return true;
    }
    long body_size = fsize - 10;

    // 如果 meta 没有 duration，先快速扫描文件计算帧数
    if (duration_sec <= 0) {
        fseek(f, 10, SEEK_SET);
        long frame_count = 0;
        uint8_t hdr[2];
        while (fread(hdr, 1, 2, f) == 2) {
            uint16_t frame_len = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
            if (frame_len == 0 || frame_len > 2000) break;
            fseek(f, frame_len, SEEK_CUR);
            frame_count++;
        }
        duration_sec = (int)((frame_count * kFrameDurationMs) / 1000);
        ESP_LOGI(TAG, "Estimated duration from %ld frames: %ds", frame_count, duration_sec);
    }

    // 流式上传：边读 SD 卡边发 HTTP，不需要把整个文件加载到内存
    // Always read the freshest token from NVS
    std::string token;
    {
        Settings s("xiaolu", false);
        token = s.GetString("device_token");
    }
    if (token.empty()) token = device_token_;

    if (server_url_.empty() || token.empty()) {
        fclose(f);
        ESP_LOGW(TAG, "No server config, cannot upload");
        return false;
    }

    std::string url = server_url_ + "/stream/upload";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 30000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 4096;
    config.skip_cert_common_name_check = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    std::string auth = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    char dur_str[16], idx_str[16];
    snprintf(dur_str, sizeof(dur_str), "%d", duration_sec);
    snprintf(idx_str, sizeof(idx_str), "%d", slice_index_);
    esp_http_client_set_header(client, "X-Audio-Duration", dur_str);
    esp_http_client_set_header(client, "X-Audio-Format", "opus");
    esp_http_client_set_header(client, "X-Slice-Index", idx_str);
    if (!current_upload_session_.empty()) {
        esp_http_client_set_header(client, "X-Upload-Session", current_upload_session_.c_str());
    }

    if (start_wall_ms > 0) {
        std::string iso = XiaoLuTime::FormatIso(start_wall_ms);
        esp_http_client_set_header(client, "X-Recording-Start-Time", iso.c_str());
    }

    ESP_LOGI(TAG, "Uploading %s (%ld bytes, %ds)", xlop_path.c_str(), body_size, duration_sec);

    // 大文件跳过（>10MB），避免长时间阻塞上传队列
    static constexpr long kMaxUploadSize = 10 * 1024 * 1024;
    if (body_size > kMaxUploadSize) {
        ESP_LOGW(TAG, "File too large (%ld bytes > 10MB), skipping: %s", body_size, xlop_path.c_str());
        esp_http_client_cleanup(client);
        fclose(f);
        return true;
    }

    // 流式上传：边读 SD 卡边发 HTTP
    esp_err_t err = esp_http_client_open(client, (int)body_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(f);
        return false;
    }

    // 从文件流式读取并发送，每次只用 4KB 缓冲区
    fseek(f, 10, SEEK_SET);
    static constexpr int kStreamBuf = 4096;
    uint8_t stream_buf[kStreamBuf];
    long total_sent = 0;
    bool write_ok = true;
    int64_t upload_start_us = esp_timer_get_time();
    static constexpr int64_t kUploadTimeoutUs = 300LL * 1000000; // 300 seconds (5 min)

    while (total_sent < body_size) {
        // 整体超时检查
        if ((esp_timer_get_time() - upload_start_us) > kUploadTimeoutUs) {
            ESP_LOGW(TAG, "Upload timeout after 120s at %ld/%ld bytes", total_sent, body_size);
            write_ok = false;
            break;
        }
        int to_read = ((body_size - total_sent) > kStreamBuf) ? kStreamBuf : (int)(body_size - total_sent);
        size_t n = fread(stream_buf, 1, to_read, f);
        if ((int)n != to_read) {
            ESP_LOGW(TAG, "SD read short at %ld", total_sent);
            write_ok = false;
            break;
        }
        int written = esp_http_client_write(client, (const char*)stream_buf, to_read);
        if (written < 0) {
            ESP_LOGE(TAG, "HTTP write failed at %ld/%ld", total_sent, body_size);
            write_ok = false;
            break;
        }
        total_sent += written;
        // 更新上传进度百分比
        upload_file_percent_ = (int)(total_sent * 100 / body_size);
        // 每 500KB 打印一次进度
        if (total_sent % (512 * 1024) < kStreamBuf) {
            ESP_LOGI(TAG, "Upload progress: %ld/%ld bytes (%d%%)",
                     total_sent, body_size, (int)(total_sent * 100 / body_size));
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(f);

    if (!write_ok) {
        ESP_LOGW(TAG, "Upload write failed for %s", xlop_path.c_str());
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG, "Upload sent %ld bytes, waiting response...", total_sent);
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    bool success = (status >= 200 && status < 300);
    ESP_LOGI(TAG, "Upload result: HTTP %d (%s)", status, success ? "OK" : "FAIL");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    slice_index_++;
    return success;
}

bool LocalStorageProtocol::NotifyUploadComplete(const std::string& upload_session_id) {
    std::string token;
    {
        Settings s("xiaolu", false);
        token = s.GetString("device_token");
    }
    if (token.empty()) token = device_token_;
    if (server_url_.empty() || token.empty()) return false;

    std::string url = server_url_ + "/stream/upload-complete";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 30000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    std::string auth = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "X-Upload-Session", upload_session_id.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload-complete: HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    bool ok = (status >= 200 && status < 300);
    ESP_LOGI(TAG, "upload-complete: HTTP %d (%s)", status, ok ? "OK" : "FAIL");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

// ─── 低电量保护 ───
// 电量低于 10% 时立即把当前切片完整落盘；充电或电量恢复时重置状态

void LocalStorageProtocol::OnBatteryUpdate(int percent, bool charging) {
    last_battery_percent_ = percent;
    is_charging_ = charging;

    // 充电或电量回升到 15% 以上：重置紧急落盘标记
    if (charging || percent >= 15) {
        emergency_flushed_ = false;
        return;
    }

    // 电量 ≤ 10% 且还没紧急落盘过 → 立即 flush
    if (percent <= 10 && !emergency_flushed_) {
        ESP_LOGW(TAG, "Low battery (%d%%), emergency flush to SD", percent);
        ForceFlushToSd();
        emergency_flushed_ = true;
    }
}

void LocalStorageProtocol::ForceFlushToSd() {
    if (has_sd_card_) {
        // SD 卡模式：把当前切片完整关闭（写 meta + fsync），然后开新切片继续录
        if (current_file_) {
            // 先 fsync 保证数据落盘
            fflush(current_file_);
            int fd = fileno(current_file_);
            if (fd >= 0) fsync(fd);
            CloseCurrentFile();
            // 继续录：开新切片（不管后续还能录多久，至少前面的已经安全了）
            if (XiaoLuTime::IsSynced()) {
                StartNewFile();
            }
        }
    } else {
        // WiFi 流式模式：触发一次上传（如果失败数据就丢了，因为没 SD 卡存）
        FlushAndUpload();
    }
}

