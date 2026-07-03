/**
 * @file freertos/semphr.h  (PC simulator stub)
 * Mutex ops are no-ops in the single-threaded SDL simulator.
 */
#pragma once
#include <stdint.h>

typedef void* SemaphoreHandle_t;

#define xSemaphoreCreateMutex()              ((SemaphoreHandle_t)1)
#define xSemaphoreTake(s, t)                 (1)   /* always succeeds */
#define xSemaphoreGive(s)                    (1)
#define xSemaphoreGiveFromISR(s, pw)         (1)
#define xSemaphoreTakeFromISR(s, pw)         (1)
#define vSemaphoreDelete(s)                  /* no-op */
