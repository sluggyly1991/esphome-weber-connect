// Saber CRC details are based on ProspectOre's MIT-licensed protocol work:
// https://github.com/ProspectOre/weber-connect-home-assistant-addon

#include "crc8.h"

namespace esphome::weber_connect_ble {

uint8_t saber_crc8(const uint8_t *data, size_t length, uint8_t initial) {
  uint8_t crc = initial;
  if (data == nullptr && length != 0)
    return crc;

  for (size_t index = 0; index < length; index++) {
    uint8_t value = data[index];
    for (uint8_t bit = 0; bit < 8; bit++) {
      const bool mix = ((crc ^ value) & 0x01U) != 0;
      crc >>= 1U;
      if (mix)
        crc ^= 0x8CU;
      value >>= 1U;
    }
  }
  return crc;
}

}  // namespace esphome::weber_connect_ble
