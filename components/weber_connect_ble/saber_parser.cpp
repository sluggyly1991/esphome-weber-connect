// Saber frame and TLV decoding is based on ProspectOre's MIT-licensed
// reverse-engineering work:
// https://github.com/ProspectOre/weber-connect-home-assistant-addon

#include "saber_parser.h"

#include <algorithm>
#include <cstring>

#include "crc8.h"

namespace esphome::weber_connect_ble {

namespace {

uint16_t read_u16_le(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t read_u32_le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

bool read_i16(const uint8_t *value, size_t length, int16_t *result) {
  if (value == nullptr || result == nullptr || length < 2)
    return false;
  *result = static_cast<int16_t>(read_u16_le(value));
  return true;
}

void copy_text(const uint8_t *value, size_t length, std::array<char, 33> *target) {
  if (target == nullptr)
    return;
  target->fill('\0');
  if (value == nullptr)
    return;
  const size_t copy_length = std::min(length, target->size() - 1);
  memcpy(target->data(), value, copy_length);
}

bool parse_probe(const uint8_t *payload, size_t length, ProbeStatus *probe) {
  if (payload == nullptr || probe == nullptr)
    return false;

  *probe = ProbeStatus{};
  bool slot_seen = false;
  bool type_19_seen = false;
  uint8_t legacy_probe_type = 0;
  size_t offset = 0;
  while (offset < length) {
    if (length - offset < 2)
      return false;
    const uint8_t tag = payload[offset];
    const size_t field_length = payload[offset + 1];
    offset += 2;
    if (field_length > length - offset)
      return false;
    const uint8_t *value = payload + offset;

    switch (tag) {
      case 1:
        if (field_length >= 1) {
          probe->slot = value[0];
          slot_seen = true;
        }
        break;
      case 4:
        if (field_length >= 1)
          legacy_probe_type = value[0];
        break;
      case 10: {
        int16_t temperature = 0;
        if (read_i16(value, field_length, &temperature)) {
          probe->temperature_deci_c = temperature;
          probe->temperature_valid = temperature != INT16_MIN;
        }
        break;
      }
      case 12:
        if (field_length >= 1)
          probe->state = value[0];
        break;
      case 19:
        if (field_length >= 1) {
          probe->probe_type = value[0];
          type_19_seen = true;
        }
        break;
      case 20:
        copy_text(value, field_length, &probe->serial);
        break;
      case 21:
        copy_text(value, field_length, &probe->sku);
        break;
      case 22:
        if (field_length >= 1) {
          probe->battery_percent = value[0];
          probe->battery_valid = true;
        }
        break;
      case 23: {
        int16_t temperature = 0;
        if (probe->segment_temperature_count < probe->segment_temperatures_deci_c.size() &&
            read_i16(value, field_length, &temperature)) {
          probe->segment_temperatures_deci_c[probe->segment_temperature_count++] = temperature;
        }
        break;
      }
      case 24: {
        int16_t temperature = 0;
        if (read_i16(value, field_length, &temperature)) {
          probe->case_temperature_deci_c = temperature;
          probe->case_temperature_valid = temperature != INT16_MIN;
        }
        break;
      }
      case 25: {
        int16_t temperature = 0;
        if (read_i16(value, field_length, &temperature)) {
          probe->ambient_temperature_deci_c = temperature;
          probe->ambient_temperature_valid = temperature != INT16_MIN;
        }
        break;
      }
      default:
        break;
    }
    offset += field_length;
  }

  if (!type_19_seen || probe->probe_type == 0)
    probe->probe_type = legacy_probe_type;
  probe->present = slot_seen && probe->slot < MAX_PROBES;
  return true;
}

}  // namespace

bool parse_saber_frame(const uint8_t *data, size_t length, SaberFrameView *frame) {
  if (data == nullptr || frame == nullptr || length < 10)
    return false;

  const uint8_t *envelope = data;
  size_t envelope_length = length;
  uint32_t sequence = 0;
  if (data[0] != 0xAB) {
    if (length < 16)
      return false;
    const uint16_t transport_length = read_u16_le(data + 4);
    if (transport_length > length - 6 || transport_length < 10)
      return false;
    sequence = read_u32_le(data);
    envelope = data + 6;
    envelope_length = transport_length;
  }
  if (envelope[0] != 0xAB || envelope[1] != 0x00 || envelope[2] != 0x00)
    return false;

  const uint16_t body_length = read_u16_le(envelope + 4);
  if (static_cast<size_t>(body_length) + 8U > envelope_length || body_length < 2)
    return false;
  const size_t crc_index = 6U + body_length;
  if (crc_index + 1 >= envelope_length || envelope[crc_index + 1] != 0x54)
    return false;

  const uint8_t expected_crc = saber_crc8(envelope + 1, 5U + body_length);
  if (expected_crc != envelope[crc_index])
    return false;

  frame->sequence = sequence;
  frame->message_version = envelope[6];
  frame->message_type = envelope[7];
  frame->payload = envelope + 8;
  frame->payload_length = body_length - 2U;
  frame->crc_ok = true;
  return true;
}

bool parse_cook_status(const uint8_t *payload, size_t length, CookStatus *status) {
  if (payload == nullptr || status == nullptr)
    return false;

  *status = CookStatus{};
  size_t offset = 0;
  while (offset < length) {
    if (length - offset < 2)
      return false;
    const uint8_t tag = payload[offset];
    const size_t field_length = payload[offset + 1];
    offset += 2;
    if (field_length > length - offset)
      return false;
    const uint8_t *value = payload + offset;

    if (tag == 4) {
      ProbeStatus probe;
      if (!parse_probe(value, field_length, &probe))
        return false;
      if (probe.present) {
        if (!status->probes[probe.slot].present)
          status->probe_count++;
        status->probes[probe.slot] = probe;
      }
    } else if (tag == 1 || tag == 2 || tag == 13) {
      int16_t temperature = 0;
      if (read_i16(value, field_length, &temperature)) {
        const bool valid = temperature != INT16_MIN;
        if (tag == 1) {
          status->target_temperature_deci_c = temperature;
          status->target_temperature_valid = valid;
        } else if (tag == 2) {
          status->display_temperature_deci_c = temperature;
          status->display_temperature_valid = valid;
        } else {
          status->actual_temperature_deci_c = temperature;
          status->actual_temperature_valid = valid;
        }
      }
    }
    offset += field_length;
  }
  return true;
}

const char *probe_state_name(uint8_t state) {
  static const char *const STATES[] = {"UNKNOWN", "IDLE",     "PROBED",   "PRIMED", "READY", "ACTIVE",
                                       "PAUSED",  "COMPLETE", "ERROR",    "ACTIVE_FIXED", "PREHEAT"};
  return state < (sizeof(STATES) / sizeof(STATES[0])) ? STATES[state] : "UNKNOWN";
}

const char *probe_type_name(uint8_t type) {
  static const char *const TYPES[] = {"UNKNOWN", "WIRED", "WIRELESS", "AMBIENT"};
  return type < (sizeof(TYPES) / sizeof(TYPES[0])) ? TYPES[type] : "UNKNOWN";
}

}  // namespace esphome::weber_connect_ble
