/**
 * @file esp_log.h  (PC simulator stub)
 * Replaces the IDF logging macros with plain printf so the screen files
 * compile on Linux/GCC without any ESP-IDF installed.
 */
#pragma once
#include <stdio.h>

#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) /* debug suppressed in sim */
#define ESP_LOGV(tag, fmt, ...) /* verbose suppressed in sim */
