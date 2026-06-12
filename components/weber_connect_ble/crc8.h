#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::weber_connect_ble {

uint8_t saber_crc8(const uint8_t *data, size_t length, uint8_t initial = 0);

}  // namespace esphome::weber_connect_ble
