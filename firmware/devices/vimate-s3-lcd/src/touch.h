// The board's touch controller (touch_st77922.c or touch_ft6236.c, see
// display.h), in the screen's coordinates.
#pragma once

#include <stdbool.h>

// Resets the controller and starts the I2C bus. false if it doesn't answer.
bool touch_init(void);

// The first touch point, if a finger is down.
bool touch_read(int *x, int *y);
