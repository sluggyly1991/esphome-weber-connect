#pragma once

// This component is based on protocol research and reverse engineering from
// ProspectOre's MIT-licensed Weber Connect Home Assistant Add-on:
// https://github.com/ProspectOre/weber-connect-home-assistant-addon

#ifdef USE_ESP32

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_gattc_api.h>

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/button/button.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "pairing.h"
#include "saber_parser.h"

namespace esphome::weber_connect_ble {

enum class WeberState : uint8_t {
  SCAN,
  CONNECT,
  DISCOVER,
  SUBSCRIBE,
  CLAIM,
  HANDSHAKE,
  PAIRING,
  LISTEN,
  DISCONNECT,
  ERROR,
  PHONE_HANDOFF,
};

enum class CharacteristicTarget : uint8_t { SESSION, COMMAND };

class WeberConnectBLE : public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_name(const std::string &name) { name_ = name; }
  void set_poll_interval(uint32_t value) { poll_interval_ms_ = value; }
  void set_listen_duration(uint32_t value) { listen_duration_ms_ = value; }
  void set_phone_handoff_duration(uint32_t value) { phone_handoff_duration_ms_ = value; }
  void set_pairing_name(const std::string &value) { pairing_name_ = value; }
  void set_handshake_characteristic(CharacteristicTarget value) { handshake_target_ = value; }
  void set_pairing_characteristic(CharacteristicTarget value) { pairing_target_ = value; }

  void set_probe_temperature_sensor(uint8_t probe, sensor::Sensor *sensor);
  void set_probe_battery_sensor(uint8_t probe, sensor::Sensor *sensor);
  void set_probe_state_sensor(uint8_t probe, text_sensor::TextSensor *sensor);
  void set_connection_state_sensor(text_sensor::TextSensor *sensor) { connection_state_sensor_ = sensor; }
  void set_hub_device_id_sensor(text_sensor::TextSensor *sensor) { hub_device_id_sensor_ = sensor; }

  void request_pairing();
  void request_disconnect();
  void request_phone_handoff();
  void clear_pairing_data();

 protected:
  void set_state_(WeberState state);
  void set_error_(const char *message);
  void reset_connection_state_();
  bool discover_handles_();
  void subscribe_characteristics_(esp_gatt_if_t gattc_if);
  void claim_session_();
  void send_handshake_();
  void send_pairing_request_();
  bool write_value_(uint16_t handle, const uint8_t *data, size_t length, esp_gatt_write_type_t write_type);
  bool write_frame_(uint16_t handle, const std::vector<uint8_t> &frame);
  void poll_response_();
  void process_frame_(const uint8_t *data, size_t length, const char *source);
  void process_status_(const SaberFrameView &frame);
  void process_pairing_response_(const SaberFrameView &frame);
  void publish_probe_status_(const CookStatus &status);
  void publish_hub_device_id_();
  void disconnect_(bool schedule_next);
  uint16_t target_handle_(CharacteristicTarget target) const;
  const char *state_name_(WeberState state) const;
  bool deadline_reached_(uint32_t deadline) const;

  std::string name_{"Weber Connect"};
  std::string pairing_name_{"ESPHome Weber"};
  uint32_t poll_interval_ms_{30000};
  uint32_t listen_duration_ms_{8000};
  uint32_t phone_handoff_duration_ms_{900000};
  CharacteristicTarget handshake_target_{CharacteristicTarget::SESSION};
  CharacteristicTarget pairing_target_{CharacteristicTarget::COMMAND};

  std::array<sensor::Sensor *, MAX_PROBES> temperature_sensors_{};
  std::array<sensor::Sensor *, MAX_PROBES> battery_sensors_{};
  std::array<text_sensor::TextSensor *, MAX_PROBES> state_sensors_{};
  text_sensor::TextSensor *connection_state_sensor_{nullptr};
  text_sensor::TextSensor *hub_device_id_sensor_{nullptr};

  WeberState state_{WeberState::ERROR};
  uint32_t state_started_at_{0};
  uint32_t next_cycle_at_{0};
  uint32_t listen_until_{0};
  uint32_t handoff_until_{0};
  uint32_t next_response_poll_at_{0};
  uint32_t sequence_{1};
  uint16_t negotiated_mtu_{23};
  bool mtu_received_{false};
  uint8_t message_version_{11};
  uint8_t subscriptions_expected_{0};
  uint8_t subscriptions_ready_{0};
  bool pair_requested_{false};
  bool pairing_request_sent_{false};
  bool status_received_{false};
  bool disconnect_schedule_next_{true};
  bool version_retry_used_{false};

  uint16_t status_handle_{0};
  uint16_t notification_handle_{0};
  uint16_t command_handle_{0};
  uint16_t session_handle_{0};
  uint16_t response_handle_{0};

  PairingData pairing_data_{};
  ESPPreferenceObject pairing_preference_;
};

class WeberPairButton : public button::Button {
 public:
  explicit WeberPairButton(WeberConnectBLE *parent) : parent_(parent) {}

 protected:
  void press_action() override { parent_->request_pairing(); }
  WeberConnectBLE *parent_;
};

class WeberDisconnectButton : public button::Button {
 public:
  explicit WeberDisconnectButton(WeberConnectBLE *parent) : parent_(parent) {}

 protected:
  void press_action() override { parent_->request_disconnect(); }
  WeberConnectBLE *parent_;
};

class WeberPhoneHandoffButton : public button::Button {
 public:
  explicit WeberPhoneHandoffButton(WeberConnectBLE *parent) : parent_(parent) {}

 protected:
  void press_action() override { parent_->request_phone_handoff(); }
  WeberConnectBLE *parent_;
};

class WeberClearPairingButton : public button::Button {
 public:
  explicit WeberClearPairingButton(WeberConnectBLE *parent) : parent_(parent) {}

 protected:
  void press_action() override { parent_->clear_pairing_data(); }
  WeberConnectBLE *parent_;
};

}  // namespace esphome::weber_connect_ble

#endif  // USE_ESP32
