/**
 * @file freertos/task.h  (PC simulator stub)
 * screen_mgr.c calls vTaskDelay(1) on the device as a task-WDT yield between
 * screen creations. The SDL simulator is single-threaded with no FreeRTOS
 * scheduler, so vTaskDelay is a no-op here.
 */
#pragma once
#include <stdint.h>

typedef uint32_t TickType_t;

#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif

#define vTaskDelay(ticks)   ((void)(ticks))   /* no-op on PC */
