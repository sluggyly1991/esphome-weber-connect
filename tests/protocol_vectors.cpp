#include <cassert>
#include <cstdint>
#include <vector>

#include "crc8.h"
#include "pairing.h"
#include "saber_parser.h"

using namespace esphome::weber_connect_ble;

namespace {

void append_u16(std::vector<uint8_t> *out, uint16_t value) {
  out->push_back(value & 0xff);
  out->push_back((value >> 8) & 0xff);
}

void append_u32(std::vector<uint8_t> *out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    out->push_back((value >> shift) & 0xff);
}

std::vector<uint8_t> make_frame(uint8_t type, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> envelope{0xab, 0x00, 0x00, 0x00};
  append_u16(&envelope, static_cast<uint16_t>(payload.size() + 2));
  envelope.push_back(11);
  envelope.push_back(type);
  envelope.insert(envelope.end(), payload.begin(), payload.end());
  envelope.push_back(saber_crc8(envelope.data() + 1, envelope.size() - 1));
  envelope.push_back(0x54);

  std::vector<uint8_t> frame;
  append_u32(&frame, 7);
  append_u16(&frame, envelope.size());
  frame.insert(frame.end(), envelope.begin(), envelope.end());
  return frame;
}

}  // namespace

int main() {
  const uint8_t maxim_vector[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  assert(saber_crc8(maxim_vector, sizeof(maxim_vector)) == 0xa1);

  const std::vector<uint8_t> probe = {
      1, 1, 0,              // slot 0
      10, 2, 0xc4, 0x09,    // 250.0 C
      12, 1, 2,             // PROBED
      19, 1, 2,             // WIRELESS
      22, 1, 88,            // battery
  };
  std::vector<uint8_t> status_payload{4, static_cast<uint8_t>(probe.size())};
  status_payload.insert(status_payload.end(), probe.begin(), probe.end());
  auto frame_bytes = make_frame(MSG_INCOMING_STATUS, status_payload);

  SaberFrameView frame;
  assert(parse_saber_frame(frame_bytes.data(), frame_bytes.size(), &frame));
  assert(frame.sequence == 7);
  assert(frame.message_type == MSG_INCOMING_STATUS);

  CookStatus status;
  assert(parse_cook_status(frame.payload, frame.payload_length, &status));
  assert(status.probe_count == 1);
  assert(status.probes[0].temperature_valid);
  assert(status.probes[0].temperature_deci_c == 2500);
  assert(status.probes[0].battery_percent == 88);
  assert(status.probes[0].state == 2);

  SaberFrameView envelope_only;
  assert(parse_saber_frame(frame_bytes.data() + 6, frame_bytes.size() - 6, &envelope_only));
  assert(envelope_only.sequence == 0);
  assert(envelope_only.message_type == MSG_INCOMING_STATUS);

  frame_bytes.back() = 0;
  assert(!parse_saber_frame(frame_bytes.data(), frame_bytes.size(), &frame));

  const uint8_t truncated_tlv[] = {4, 5, 1, 1};
  assert(!parse_cook_status(truncated_tlv, sizeof(truncated_tlv), &status));

  PairingData pairing;
  for (size_t index = 0; index < pairing.companion_id.size(); index++)
    pairing.companion_id[index] = static_cast<uint8_t>(index);
  pairing.companion_public_key.fill(0xaa);
  auto pairing_frame = build_pairing_frame(1, 11, pairing, "ESPHome Weber");
  assert(pairing_frame.size() == 110);
  SaberFrameView pairing_view;
  assert(parse_saber_frame(pairing_frame.data(), pairing_frame.size(), &pairing_view));
  assert(pairing_view.message_type == MSG_OUTGOING_PAIRING_REQUEST);
  assert(pairing_view.payload_length == 94);

  std::array<uint8_t, 32> nonce{};
  auto handshake = build_handshake_frame(2, 11, pairing.companion_id, nonce);
  SaberFrameView handshake_view;
  assert(parse_saber_frame(handshake.data(), handshake.size(), &handshake_view));
  assert(handshake_view.message_type == MSG_OUTGOING_HANDSHAKE);
  assert(handshake_view.payload_length == 48);
  return 0;
}
