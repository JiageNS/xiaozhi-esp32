#include "xiaolu_time.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>
#include <ctime>

#define TAG "XLTime"

namespace XiaoLuTime {

static bool g_synced = false;
static int64_t g_offset_ms = 0;  // wall_ms - monotonic_ms

static int64_t MonotonicMs() {
    return esp_timer_get_time() / 1000;
}

// 解析 YYYY-MM-DDTHH:MM:SS（北京时区，无 Z 后缀）为北京墙钟 unix ms
static int64_t ParseServerTime(const char* s) {
    if (!s) return 0;
    int y, mo, d, h, mi, se;
    int n = sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
    if (n < 6) return 0;

    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = se;
    // 服务器字符串是北京时间字面值，我们要把它当 UTC 转一次再 +8h 的差就抵消了
    // 最简单：设 TZ=UTC 然后用 timegm / mktime
    // ESP-IDF 默认 TZ 是 UTC，mktime 也当 UTC 处理，所以直接 mktime 得到的就是"北京时间对应的 unix 秒"（偏了 8h）
    // 我们要的"北京墙钟的 unix ms" = 真实 UTC unix ms + 8*3600*1000，但业务里所有时间都按这个偏移
    // 既然所有对比都在北京时区，这里直接把"假装 UTC 的 mktime 结果"当墙钟用即可
    time_t secs = mktime(&t);
    if (secs < 0) return 0;
    return (int64_t)secs * 1000;
}

void SyncFromServerTime(const char* server_time_str) {
    int64_t wall_ms = ParseServerTime(server_time_str);
    if (wall_ms <= 0) {
        ESP_LOGW(TAG, "Invalid server_time: %s", server_time_str ? server_time_str : "(null)");
        return;
    }
    int64_t mono_ms = MonotonicMs();
    int64_t new_offset = wall_ms - mono_ms;

    if (!g_synced) {
        g_offset_ms = new_offset;
        g_synced = true;
        ESP_LOGI(TAG, "Time synced: %s (offset=%s ms)", server_time_str, FormatI64(new_offset).c_str());
    } else {
        // 已同步过：做个平滑，避免每次都跳变
        int64_t diff = new_offset - g_offset_ms;
        if (diff > 2000 || diff < -2000) {
            // 差异超过 2 秒才认，避免网络抖动引起漂移
            g_offset_ms = new_offset;
            ESP_LOGI(TAG, "Time resynced: %s (offset=%s ms, jump=%s)", server_time_str, FormatI64(new_offset).c_str(), FormatI64(diff).c_str());
        }
    }
}

bool IsSynced() { return g_synced; }

int64_t WallNowMs() {
    if (!g_synced) return 0;
    return MonotonicMs() + g_offset_ms;
}

int64_t WallNowSec() {
    int64_t ms = WallNowMs();
    return ms / 1000;
}

static void BreakDown(int64_t wall_ms, struct tm& out) {
    time_t secs = (time_t)(wall_ms / 1000);
    gmtime_r(&secs, &out);  // 用 gmtime 因为 wall_ms 本身就是"北京时间字面值对应的假UTC秒"
}

std::string FormatIso(int64_t wall_ms) {
    struct tm t;
    BreakDown(wall_ms, t);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

std::string FormatHourBucket(int64_t wall_ms) {
    struct tm t;
    BreakDown(wall_ms, t);
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    return buf;
}

std::string FormatHms(int64_t wall_ms) {
    struct tm t;
    BreakDown(wall_ms, t);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d%02d%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

// 手动把 int64_t 转成十进制字符串。
// ESP-IDF 默认开启 CONFIG_NEWLIB_NANO_FORMAT=y，nano-printf/scanf 不支持 %lld/%llu，
// 直接用 %lld 会把 "ld" 字面量当输出，并且不消费 va_arg，导致后续 %s 读取错位的指针崩溃。
std::string FormatI64(int64_t v) {
    char buf[24];
    bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    int idx = sizeof(buf);
    buf[--idx] = 0;
    if (u == 0) {
        buf[--idx] = '0';
    } else {
        while (u > 0 && idx > 0) {
            buf[--idx] = '0' + (u % 10);
            u /= 10;
        }
    }
    if (neg && idx > 0) buf[--idx] = '-';
    return std::string(&buf[idx]);
}

} // namespace XiaoLuTime
