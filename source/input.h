/**
 * @file input.h
 * @brief PS Vita front multitouch and physical controller mapping for iOS touch events.
 */
#pragma once
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <stdint.h>

/**
 * @brief Represents an active touch point mapped from Vita hardware to guest coordinates.
 */
typedef struct {
    int id;   /**< Unique touch report identifier. */
    int x;    /**< Raw X coordinate in PS Vita screen space (0..960). */
    int y;    /**< Raw Y coordinate in PS Vita screen space (0..544). */
    int gx;   /**< Projected X coordinate in guest screen space (default 480x320 landscape or configurable). */
    int gy;   /**< Projected Y coordinate in guest screen space. */
    int down; /**< 1 if finger is touching screen, 0 if released. */
} touch_point;

extern touch_point touches[SCE_TOUCH_MAX_REPORT];
extern int touch_count;
extern SceCtrlData pad;

/**
 * @brief Initializes front touch panel and analog joystick sampling modes.
 */
void input_init(void);

/**
 * @brief Polls physical button state and updates projected touch positions.
 */
void input_poll(void);
