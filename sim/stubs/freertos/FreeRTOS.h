/**
 * @file freertos/FreeRTOS.h  (PC simulator stub)
 * scr_scan.c includes FreeRTOS/semphr for its internal ring-buffer mutex.
 * In the simulator there is only one thread (SDL event loop), so all mutex
 * ops are no-ops.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Minimal type + macro stubs so scr_scan.c compiles without FreeRTOS */
#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffUL
typedef void* SemaphoreHandle_t;

/* FreeRTOS.h normally pulls in semphr.h via portmacro — replicate that here */
#include "semphr.h"
