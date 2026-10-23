// Deck Link over Bluetooth LE: the same byte stream as on USB, through a
// Nordic-UART-style GATT service (write to RX, notifications from TX). The
// Mac side is tools/ble_bridge.py, which hands it to the daemon as a serial
// port. No pairing: anyone nearby can connect, but the deck still has to
// approve the board, and the board can only do what its scope allows.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

// Starts advertising as name.
void ble_start(const char *name);

// A central is connected and listening to TX.
bool ble_connected(void);

// Reads what the central wrote, waiting up to wait ticks.
size_t ble_read(uint8_t *buf, size_t len, TickType_t wait);

// Sends data as notifications; returns how much went out.
size_t ble_write(const uint8_t *data, size_t len);
