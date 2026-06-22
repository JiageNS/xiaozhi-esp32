#ifndef XIAOLU_TIME_H
#define XIAOLU_TIME_H

#include <string>
#include <cstdint>

/**
 * XiaoLuTime — 小鹿豆墙钟时间服务
 *
 * 核心思路：心跳响应里有 server_time（格式: YYYY-MM-DDTHH:MM:SS，北京时区）
 *   把它和本机单调时钟 esp_timer_get_time() 绑定
 *   以后任何时候都能算出"现在的真实北京时间"
 *
 * offset_ms = wall_ms(server) - monotonic_ms(local, at sync)
 * wall_now_ms() = monotonic_ms() + offset_ms
 *
 * 未同步前：返回 0，调用方需要判断。
 */
namespace XiaoLuTime {

/** 心跳收到 server_time 字符串时调用（YYYY-MM-DDTHH:MM:SS 北京时区） */
void SyncFromServerTime(const char* server_time_str);

/** 是否已同步过至少一次 */
bool IsSynced();

/** 当前北京墙钟时间（毫秒） */
int64_t WallNowMs();

/** 当前北京墙钟时间（秒） */
int64_t WallNowSec();

/** 把毫秒时间戳格式化为 YYYY-MM-DDTHH:MM:SS（北京时区）*/
std::string FormatIso(int64_t wall_ms);

/** 把毫秒时间戳格式化为 YYYYMMDD_HH（小时桶） */
std::string FormatHourBucket(int64_t wall_ms);

/** 把毫秒时间戳格式化为 HHMMSS */
std::string FormatHms(int64_t wall_ms);

/** 把 int64_t 格式化为十进制字符串（避免依赖 newlib-nano 不支持的 %lld）*/
std::string FormatI64(int64_t v);

} // namespace XiaoLuTime

#endif
