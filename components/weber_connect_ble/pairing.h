#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::weber_connect_ble {

constexpr uint32_t PAIRING_DATA_MAGIC = 0x5743424CU;
constexpr uint8_t PAIRING_DATA_VERSION = 1;

struct PairingData {
  uint32_t magic{PAIRING_DATA_MAGIC};
  uint8_t version{PAIRING_DATA_VERSION};
  uint8_t confirmed{0};
  std::array<uint8_t, 16> companion_id{};
  std::array<uint8_t, 32> companion_private_key{};
  std::array<uint8_t, 64> companion_public_key{};
  std::array<uint8_t, 16> appliance_id{};
  std::array<uint8_t, 64> appliance_public_key{};
};

bool pairing_data_valid(const PairingData &data);
bool pairing_material_valid(const PairingData &data);
bool generate_pairing_data(PairingData *data);
bool fill_random(uint8_t *data, size_t length);
void clear_pairing_data(PairingData *data);

std::vector<uint8_t> build_command_frame(uint32_t sequence, uint8_t message_version, uint8_t message_type,
                                         const uint8_t *payload, size_t payload_length);
std::vector<uint8_t> build_handshake_frame(uint32_t sequence, uint8_t message_version,
                                           const std::array<uint8_t, 16> &companion_id,
                                           const std::array<uint8_t, 32> &nonce);
std::vector<uint8_t> build_pairing_frame(uint32_t sequence, uint8_t message_version, const PairingData &data,
                                         const std::string &display_name);

}  // namespace esphome::weber_connect_ble
