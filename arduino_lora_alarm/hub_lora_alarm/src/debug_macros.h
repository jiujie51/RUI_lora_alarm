/*
 * 调试宏 — SEGGER RTT 输出 (适用于无串口的硬件)
 *
 * 使用: LOG_INFO("tag", "fmt", args...);
 *       LOG_WARN("tag", "fmt", args...);
 *       LOG_ERROR("tag", "fmt", args...);
 *
 * 输出通道: SEGGER_RTT channel 0 (J-Link SWD / RTT Viewer)
 * PC 端工具: JLinkRTTViewer 或 nRF Connect → RTT Viewer
 *
 * 如有串口, 改用:
 *   #define LOG_INFO(tag, fmt, ...) Serial.printf("[%s] " fmt "\r\n", tag, ##__VA_ARGS__)
 *
 * 依赖: nRF5_SDK external/segger_rtt (RUI3 已内置, 路径在 boards.txt 已配置)
 */
#ifndef DEBUG_MACROS_H
#define DEBUG_MACROS_H

#include "SEGGER_RTT.h"

#define ENABLE_SERIAL_DEBUG 1

#ifdef ENABLE_SERIAL_DEBUG
  #define LOG_INFO(tag, fmt, ...)   SEGGER_RTT_printf(0, "[%s] " fmt "\r\n", tag, ##__VA_ARGS__)
  #define LOG_WARN(tag, fmt, ...)   SEGGER_RTT_printf(0, "[%s] WARN: " fmt "\r\n", tag, ##__VA_ARGS__)
  #define LOG_ERROR(tag, fmt, ...)  SEGGER_RTT_printf(0, "[%s] ERR: " fmt "\r\n", tag, ##__VA_ARGS__)
#else
  #define LOG_INFO(tag, fmt, ...)   do {} while(0)
  #define LOG_WARN(tag, fmt, ...)   do {} while(0)
  #define LOG_ERROR(tag, fmt, ...)  do {} while(0)
#endif

#endif /* DEBUG_MACROS_H */
