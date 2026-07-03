/**
 * @file lv_conf.h
 * LVGL v9.5.0 configuration for uBIOPOT PC Simulator (lv_port_pc_vscode / SDL2).
 *
 * Mirrors the firmware's sdkconfig.defaults LVGL options so screen code is
 * 100% portable between the ESP32 target and the desktop simulator.
 *
 * Key matches with firmware:
 *   LV_COLOR_DEPTH 16     — RGB565, matches ILI9341 + firmware swap_bytes
 *   Montserrat 14/20/28   — same fonts used in all screens
 *   LV_USE_OS LV_OS_NONE  — PC sim uses SDL event loop, not FreeRTOS
 *   LV_MEM_SIZE 128 KB    — generous for desktop; device uses heap_caps
 *
 * Place this file next to lv_port_pc_vscode/CMakeLists.txt and set:
 *   set(LV_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/../lv_conf.h CACHE STRING "" FORCE)
 * or copy it into the lv_port_pc_vscode root (next to the lvgl/ folder).
 */

/* clang-format off */
#if 1   /* << ENABLED (changed from template's 0) */

#ifndef LV_CONF_H
#define LV_CONF_H

#if  0 && defined(__ASSEMBLY__)
/* nothing */
#endif

/* ======================================================================
   COLOR
   ====================================================================== */
/** 16 = RGB565, matches ILI9341 and firmware partial-render buffers. */
#define LV_COLOR_DEPTH 16

/* ======================================================================
   STDLIB
   ====================================================================== */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#define LV_STDINT_INCLUDE   <stdint.h>
#define LV_STDDEF_INCLUDE   <stddef.h>
#define LV_STDBOOL_INCLUDE  <stdbool.h>
#define LV_INTTYPES_INCLUDE <inttypes.h>
#define LV_LIMITS_INCLUDE   <limits.h>
#define LV_STDARG_INCLUDE   <stdarg.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    /** 128 KB — generous for the PC sim (device uses ESP heap_caps). */
    #define LV_MEM_SIZE          (128U * 1024U)
    #define LV_MEM_POOL_EXPAND_SIZE 0
    #define LV_MEM_ADR           0
    #if LV_MEM_ADR == 0
        #define LV_MEM_POOL_ALLOC
    #endif
#endif

/* ======================================================================
   OS / THREADING
   PC sim: LV_OS_NONE — SDL event loop drives lv_timer_handler.
   Device: LV_OS_FREERTOS (set via sdkconfig CONFIG_LV_OS_FREERTOS=y).
   ====================================================================== */
#define LV_USE_OS   LV_OS_NONE

#if LV_USE_OS == LV_OS_CUSTOM
    #define LV_OS_CUSTOM_INCLUDE <stdint.h>
#endif
#if LV_USE_OS == LV_OS_FREERTOS
    #define LV_USE_FREERTOS_TASK_NOTIFY 1
#endif

/* ======================================================================
   RENDERING ENGINE
   ====================================================================== */
#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1
    #define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
    #define LV_DRAW_SW_DRAW_UNIT_CNT 1
    #define LV_USE_DRAW_SW_COMPLEX 1
    #define LV_DRAW_SW_SHADOW_CACHE_SIZE 0
    #define LV_DRAW_SW_CIRCLE_CACHE_SIZE 4
#endif

/* ======================================================================
   DISPLAY
   ====================================================================== */
#define LV_DISP_DEF_REFR_PERIOD  16 /* ~60 fps on PC */
#define LV_DPI_DEF               130

/* ======================================================================
   ROTATION
   ====================================================================== */
#define LV_USE_ROTATION 1

/* ======================================================================
   ANIMATION
   ====================================================================== */
#define LV_USE_ANIM 1

/* ======================================================================
   SCROLL
   ====================================================================== */
#define LV_USE_SCROLL_ON_FOCUS 1

/* ======================================================================
   INDEV
   ====================================================================== */
#define LV_DEF_REFR_PERIOD   16
#define LV_INDEV_DEF_READ_PERIOD 16

/* ======================================================================
   LOGGING
   Enable on PC sim so we can see LVGL warnings while developing screens.
   ====================================================================== */
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF   1
    #define LV_LOG_USE_TIMESTAMP 1
    #define LV_LOG_USE_FUNC_NAME 1
#endif

/* ======================================================================
   ASSERT
   ====================================================================== */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/* ======================================================================
   FONTS — enable the three sizes used in uBIOPOT screens.
   ====================================================================== */

/* Montserrat 14 — body text, status bar, hints */
#define LV_FONT_MONTSERRAT_8   0
#define LV_FONT_MONTSERRAT_10  0
#define LV_FONT_MONTSERRAT_12  0
#define LV_FONT_MONTSERRAT_14  1   /* << ENABLED */
#define LV_FONT_MONTSERRAT_16  0
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  1   /* << ENABLED — menu items, readout labels */
#define LV_FONT_MONTSERRAT_22  0
#define LV_FONT_MONTSERRAT_24  0
#define LV_FONT_MONTSERRAT_26  0
#define LV_FONT_MONTSERRAT_28  1   /* << ENABLED — peak values, splash logo */
#define LV_FONT_MONTSERRAT_30  0
#define LV_FONT_MONTSERRAT_32  0
#define LV_FONT_MONTSERRAT_34  0
#define LV_FONT_MONTSERRAT_36  0
#define LV_FONT_MONTSERRAT_38  0
#define LV_FONT_MONTSERRAT_40  0
#define LV_FONT_MONTSERRAT_42  0
#define LV_FONT_MONTSERRAT_44  0
#define LV_FONT_MONTSERRAT_46  0
#define LV_FONT_MONTSERRAT_48  0

/* Montserrat compressed variants */
#define LV_FONT_MONTSERRAT_12_SUBPX        0
#define LV_FONT_MONTSERRAT_28_COMPRESSED   0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW   0
#define LV_FONT_SIMSUN_14_CJK              0
#define LV_FONT_SIMSUN_16_CJK              0

/* Built-in monospace */
#define LV_FONT_UNSCII_8   0
#define LV_FONT_UNSCII_16  0

/* Default font — must be one that is enabled above. */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Custom font support */
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_PLACEHOLDER 1

/* ======================================================================
   TEXT SETTINGS
   ====================================================================== */
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/* ======================================================================
   WIDGETS — enable all widgets used in uBIOPOT screens.
   ====================================================================== */
#define LV_USE_ANIMIMG      0
#define LV_USE_ARC          1  /* equilibration spinner arc */
#define LV_USE_ARCLABEL     1
#define LV_USE_BAR          1  /* progress bar on splash */
#define LV_USE_BUTTON       1  /* "Run Again", menu items */
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR     0
#define LV_USE_CANVAS       1  /* required by lv_qrcode */
#define LV_USE_CHART        1  /* voltammogram! */
#define LV_USE_CHECKBOX     0
#define LV_USE_DROPDOWN     0
#define LV_USE_IMAGE        0
#define LV_USE_IMAGEBUTTON  0
#define LV_USE_KEYBOARD     0
#define LV_USE_LABEL        1  /* everywhere */
#define LV_USE_LED          0
#define LV_USE_LINE         1
#define LV_USE_LIST         1  /* home menu */
#define LV_USE_MENU         0
#define LV_USE_MSGBOX       0
#define LV_USE_ROLLER       0
#define LV_USE_SCALE        1
#define LV_USE_SLIDER       0
#define LV_USE_SPAN         0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      1  /* equilibration "loading" spinner */
#define LV_USE_SWITCH       0
#define LV_USE_TEXTAREA     0
#define LV_USE_TABLE        0
#define LV_USE_TABVIEW      0
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/* ======================================================================
   EXTRA THEMES / LAYOUTS
   ====================================================================== */
#define LV_USE_FLEX   1
#define LV_USE_GRID   0

/* Built-in themes */
#define LV_USE_THEME_DEFAULT 1   /* we apply manual dark palette on top */
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK 1    /* start dark so base objects are dark */
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_SIMPLE  1
#define LV_USE_THEME_MONO    0

/* ======================================================================
   EXTRA COMPONENTS
   ====================================================================== */
#define LV_USE_QRCODE        1   /* settings screen QR code */
#define LV_USE_BARCODE       0
#define LV_USE_SNAPSHOT      0
#define LV_USE_SYSMON        0
#define LV_USE_PROFILER      0
#define LV_USE_MONKEY        0
#define LV_USE_GRIDNAV       0
#define LV_USE_FRAGMENT      0
#define LV_USE_IMGFONT       0
#define LV_USE_OBSERVER      0
#define LV_USE_IME_PINYIN    0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_FONT_MANAGER  0

/* ======================================================================
   SDL2 SIMULATOR CONFIG (lv_port_pc_vscode specific)
   ====================================================================== */
#define LV_USE_SDL 1
#if LV_USE_SDL
    #define LV_SDL_INCLUDE_PATH  <SDL2/SDL.h>
    #define LV_SDL_RENDER_MODE   LV_DISPLAY_RENDER_MODE_PARTIAL
    #define LV_SDL_BUF_COUNT     2       /* double-buffer like device */
    #define LV_SDL_FULLSCREEN    0
    #define LV_SDL_DIRECT_EXIT   1
    #define LV_SDL_MOUSEWHEEL_MODE LV_SDL_MOUSEWHEEL_MODE_ENCODER
#endif

/* ======================================================================
   GPU / DRAW EXTENSIONS (all off for portable sim)
   ====================================================================== */
#define LV_USE_DRAW_VGLITE  0
#define LV_USE_DRAW_PXP     0
#define LV_USE_DRAW_G2D     0
#define LV_USE_DRAW_DMA2D   0
#define LV_USE_DRAW_DAVE2D  0
#define LV_USE_NEMA_GFX     0
#define LV_USE_VECTOR_ANIM  0

/* ======================================================================
   DISPLAY DRIVER (non-SDL path, unused on PC but needs a value)
   ====================================================================== */
#define LV_USE_LINUX_FBDEV  0
#define LV_USE_LINUX_DRM    0
#define LV_USE_OPENGLES     0

/* ======================================================================
   MISCELLANEOUS
   ====================================================================== */
#define LV_SPRINTF_CUSTOM    0
#define LV_USE_USER_DATA     1
#define LV_USE_PERF_MONITOR  0
#define LV_USE_MEM_MONITOR   0
#define LV_USE_FLOAT         0   /* chart values are int32_t, not float */

#define LV_ATTRIBUTE_FAST_MEM
#define LV_EXPORT_CONST_INT(int_value) struct _silence_iso_pedantic_warning
#define LV_ATTRIBUTE_EXTERN_DATA

#endif /* LV_CONF_H */
#endif /* Enable content */
