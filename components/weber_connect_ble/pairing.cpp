// Protocol framing and pairing key generation are derived from the MIT-licensed
// reverse-engineering work by ProspectOre:
// https://github.com/ProspectOre/weber-connect-home-assistant-addon

#include "pairing.h"

#include <algorithm>
#include <cstring>

#ifdef USE_ESP32
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#else
#include <random>
#endif

#include "crc8.h"
#include "saber_parser.h"

namespace esphome::weber_connect_ble {

namespace {

void append_u16_le(std::vector<uint8_t> *output, uint16_t value) {
  output->push_back(static_cast<uint8_t>(value & 0xFFU));
  output->push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32_le(std::vector<uint8_t> *output, uint32_t value) {
  for (uint8_t shift = 0; shift < 32; shift += 8)
    output->push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
}

}  // namespace

bool pairing_data_valid(const PairingData &data) {
  return pairing_material_valid(data) && data.confirmed == 1;
}

bool pairing_material_valid(const PairingData &data) {
  if (data.magic != PAIRING_DATA_MAGIC || data.version != PAIRING_DATA_VERSION)
    return false;
  return std::any_of(data.companion_id.begin(), data.companion_id.end(), [](uint8_t value) { return value != 0; }) &&
         std::any_of(data.companion_public_key.begin(), data.companion_public_key.end(),
                     [](uint8_t value) { return value != 0; });
}

bool fill_random(uint8_t *data, size_t length) {
  if (data == nullptr && length != 0)
    return false;

#ifdef USE_ESP32
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  static constexpr char PERSONALIZATION[] = "esphome-weber-connect";
  int result = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char *>(PERSONALIZATION),
                                    sizeof(PERSONALIZATION) - 1);
  if (result == 0)
    result = mbedtls_ctr_drbg_random(&ctr_drbg, data, length);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  return result == 0;
#else
  std::random_device random;
  for (size_t index = 0; index < length; index++)
    data[index] = static_cast<uint8_t>(random());
  return true;
#endif
}

bool generate_pairing_data(PairingData *data) {
  if (data == nullptr)
    return false;

#ifdef USE_ESP32
  PairingData generated;
  if (!fill_random(generated.companion_id.data(), generated.companion_id.size()))
    return false;

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ecp_keypair keypair;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_ecp_keypair_init(&keypair);
  static constexpr char PERSONALIZATION[] = "weber-p256-key";
  int result = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char *>(PERSONALIZATION),
                                    sizeof(PERSONALIZATION) - 1);
  if (result == 0)
    result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &keypair, mbedtls_ctr_drbg_random, &ctr_drbg);
  if (result == 0)
    result = mbedtls_mpi_write_binary(&keypair.MBEDTLS_PRIVATE(d), generated.companion_private_key.data(),
                                     generated.companion_private_key.size());
  if (result == 0)
    result = mbedtls_mpi_write_binary(&keypair.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
                                     generated.companion_public_key.data(), 32);
  if (result == 0)
    result = mbedtls_mpi_write_binary(&keypair.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y),
                                     generated.companion_public_key.data() + 32, 32);

  mbedtls_ecp_keypair_free(&keypair);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  if (result != 0)
    return false;

  *data = generated;
  return true;
#else
  return false;
#endif
}

void clear_pairing_data(PairingData *data) {
  if (data == nullptr)
    return;
  volatile uint8_t *bytes = reinterpret_cast<volatile uint8_t *>(data);
  for (size_t index = 0; index < sizeof(PairingData); index++)
    bytes[index] = 0;
  *data = PairingData{};
}

std::vector<uint8_t> build_command_frame(uint32_t sequence, uint8_t message_version, uint8_t message_type,
                                         const uint8_t *payload, size_t payload_length) {
  if ((payload == nullptr && payload_length != 0) || payload_length > UINT16_MAX - 10U)
    return {};

  const uint16_t body_length = static_cast<uint16_t>(payload_length + 2U);
  std::vector<uint8_t> envelope;
  envelope.reserve(body_length + 8U);
  envelope.push_back(0xAB);
  envelope.push_back(0x00);
  envelope.push_back(0x00);
  envelope.push_back(0x00);
  append_u16_le(&envelope, body_length);
  envelope.push_back(message_version);
  envelope.push_back(message_type);
  if (payload_length != 0)
    envelope.insert(envelope.end(), payload, payload + payload_length);
  envelope.push_back(saber_crc8(envelope.data() + 1, envelope.size() - 1));
  envelope.push_back(0x54);

  std::vector<uint8_t> frame;
  frame.reserve(envelope.size() + 6U);
  append_u32_le(&frame, sequence);
  append_u16_le(&frame, static_cast<uint16_t>(envelope.size()));
  frame.insert(frame.end(), envelope.begin(), envelope.end());
  return frame;
}

std::vector<uint8_t> build_handshake_frame(uint32_t sequence, uint8_t message_version,
                                           const std::array<uint8_t, 16> &companion_id,
                                           const std::array<uint8_t, 32> &nonce) {
  std::array<uint8_t, 48> payload{};
  std::copy(companion_id.begin(), companion_id.end(), payload.begin());
  std::copy(nonce.begin(), nonce.end(), payload.begin() + companion_id.size());
  return build_command_frame(sequence, message_version, MSG_OUTGOING_HANDSHAKE, payload.data(), payload.size());
}

std::vector<uint8_t> build_pairing_frame(uint32_t sequence, uint8_t message_version, const PairingData &data,
                                         const std::string &display_name) {
  std::string name = display_name;
  while (name.size() > 32) {
    size_t codepoint_length = 1;
    const uint8_t first = static_cast<uint8_t>(name[0]);
    if ((first & 0xE0U) == 0xC0U)
      codepoint_length = 2;
    else if ((first & 0xF0U) == 0xE0U)
      codepoint_length = 3;
    else if ((first & 0xF8U) == 0xF0U)
      codepoint_length = 4;
    name.erase(0, std::min(codepoint_length, name.size()));
  }

  std::vector<uint8_t> payload;
  payload.reserve(81U + name.size());
  payload.insert(payload.end(), data.companion_id.begin(), data.companion_id.end());
  payload.insert(payload.end(), data.companion_public_key.begin(), data.companion_public_key.end());
  payload.push_back(static_cast<uint8_t>(name.size()));
  payload.insert(payload.end(), name.begin(), name.end());
  return build_command_frame(sequence, message_version, MSG_OUTGOING_PAIRING_REQUEST, payload.data(), payload.size());
}

}  // namespace esphome::weber_connect_ble
