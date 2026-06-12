#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::weber_connect_ble {

constexpr uint8_t MSG_INCOMING_STATUS = 0x80;
constexpr uint8_t MSG_INCOMING_PAIRING_RESPONSE = 0x85;
constexpr uint8_t MSG_INCOMING_ERROR = 0x87;
constexpr uint8_t MSG_INCOMING_PAIRING_REQUIRED = 0xF1;
constexpr uint8_t MSG_INCOMING_HANDSHAKE_SUCCESS = 0xF2;
constexpr uint8_t MSG_OUTGOING_PAIRING_REQUEST = 0x0A;
constexpr uint8_t MSG_OUTGOING_HANDSHAKE = 0x70;
constexpr size_t MAX_PROBES = 4;
constexpr size_t MAX_SEGMENT_TEMPERATURES = 8;

struct SaberFrameView {
  uint32_t sequence{0};
  uint8_t message_version{0};
  uint8_t message_type{0};
  const uint8_t *payload{nullptr};
  size_t payload_length{0};
  bool crc_ok{false};
};

struct ProbeStatus {
  bool present{false};
  uint8_t slot{0};
  bool temperature_valid{false};
  int16_t temperature_deci_c{0};
  bool battery_valid{false};
  uint8_t battery_percent{0};
  uint8_t state{0};
  uint8_t probe_type{0};
  bool case_temperature_valid{false};
  int16_t case_temperature_deci_c{0};
  bool ambient_temperature_valid{false};
  int16_t ambient_temperature_deci_c{0};
  std::array<int16_t, MAX_SEGMENT_TEMPERATURES> segment_temperatures_deci_c{};
  size_t segment_temperature_count{0};
  std::array<char, 33> serial{};
  std::array<char, 33> sku{};
};

struct CookStatus {
  std::array<ProbeStatus, MAX_PROBES> probes{};
  size_t probe_count{0};
  bool target_temperature_valid{false};
  int16_t target_temperature_deci_c{0};
  bool display_temperature_valid{false};
  int16_t display_temperature_deci_c{0};
  bool actual_temperature_valid{false};
  int16_t actual_temperature_deci_c{0};
};

bool parse_saber_frame(const uint8_t *data, size_t length, SaberFrameView *frame);
bool parse_cook_status(const uint8_t *payload, size_t length, CookStatus *status);
const char *probe_state_name(uint8_t state);
const char *probe_type_name(uint8_t type);

}  // namespace esphome::weber_connect_ble
