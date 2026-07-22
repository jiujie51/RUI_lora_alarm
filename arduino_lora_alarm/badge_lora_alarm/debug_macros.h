/*
 * 调试宏 — 替代 Zephyr LOG_MODULE_REGISTER / LOG_INF / LOG_WRN / LOG_ERR
 *
 * 使用: LOG_INFO("tag", "fmt", args...);
 *       LOG_WARN("tag", "fmt", args...);
 *       LOG_ERROR("tag", "fmt", args...);
 *
 * 注意: %llu/%lld 等 64-bit 格式在 RUI3 printf 中可能不完整支持,
 *       必要时拆分为两个 %lu 或用 PRIu32 处理
 */
#ifndef DEBUG_MACROS_H
#define DEBUG_MACROS_H

#ifdef ENABLE_SERIAL_DEBUG
  #define LOG_INFO(tag, fmt, ...)   Serial.printf("[%s] " fmt "\r\n", tag, ##__VA_ARGS__)
  #define LOG_WARN(tag, fmt, ...)   Serial.printf("[%s] WARN: " fmt "\r\n", tag, ##__VA_ARGS__)
  #define LOG_ERROR(tag, fmt, ...)  Serial.printf("[%s] ERR: " fmt "\r\n", tag, ##__VA_ARGS__)
#else
  #define LOG_INFO(tag, fmt, ...)   do {} while(0)
  #define LOG_WARN(tag, fmt, ...)   do {} while(0)
  #define LOG_ERROR(tag, fmt, ...)  do {} while(0)
#endif

#endif /* DEBUG_MACROS_H */
