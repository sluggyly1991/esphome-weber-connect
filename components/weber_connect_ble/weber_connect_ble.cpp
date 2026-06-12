#include "weber_connect_ble.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/core/log.h"

namespace esphome::weber_connect_ble {

static const char *const TAG = "weber_connect_ble";
static constexpr uint32_t PAIRING_PREFERENCE_KEY = 0x57454252U;
static constexpr uint16_t REQUIRED_MINIMUM_MTU = 114;

static const esp32_ble_tracker::ESPBTUUID APPLIANCE_SERVICE_UUID =
    esp32_ble_tracker::ESPBTUUID::from_raw("01014a75-6e65-81de-a4b2-940b4c6f6b69");
static const esp32_ble_tracker::ESPBTUUID STATUS_UUID =
    esp32_ble_tracker::ESPBTUUID::from_raw("31014a75-6e65-81de-a4b2-940b4c6f6b69");
static const esp32_ble_tracker::ESPBTUUID NOTIFICATION_UUID =
    esp32_ble_tracker::ESPBTUUID::from_raw("31024a75-6e65-81de-a4b2-940b4c6f6b69");
static const esp32_ble_tracker::ESPBTUUID COMMAND_UUID =
    esp32_ble_tracker::ESPBTUUID::from_raw("31034a75-6e65-81de-a4b2-940b4c6f6b69");
static const esp32_ble_tracker::ESPBTUUID SESSION_UUID =
    esp32_ble_tracker::ESPBTUUID::from_raw("31044a75-6e65-81de-a4b2-940b4c6f6b69");
static const esp32_ble_tracker::ESPBTUUID RESPONSE_UUID =
    esp32_ble_tracker::ESPBTUUID::from_raw("31084a75-6e65-81de-a4b2-940b4c6f6b69");

void WeberConnectBLE::setup() {
  this->pairing_preference_ = global_preferences->make_preference<PairingData>(PAIRING_PREFERENCE_KEY, true);
  PairingData stored;
  if (this->pairing_preference_.load(&stored) && pairing_material_valid(stored)) {
    this->pairing_data_ = stored;
    ESP_LOGI(TAG, "Loaded %s pairing data", pairing_data_valid(stored) ? "confirmed" : "pending");
  } else {
    clear_pairing_data(&this->pairing_data_);
    ESP_LOGI(TAG, "No pairing data stored; use the Pair button to enroll this ESP32");
  }
  this->publish_hub_device_id_();
  this->set_state_(WeberState::SCAN);
  this->next_cycle_at_ = millis();
}

void WeberConnectBLE::dump_config() {
  ESP_LOGCONFIG(TAG, "Weber Connect BLE:");
  ESP_LOGCONFIG(TAG, "  Name: %s", this->name_.c_str());
  ESP_LOGCONFIG(TAG, "  Poll interval: %.1fs", this->poll_interval_ms_ / 1000.0f);
  ESP_LOGCONFIG(TAG, "  Listen duration: %.1fs", this->listen_duration_ms_ / 1000.0f);
  ESP_LOGCONFIG(TAG, "  Phone handoff duration: %.1fs", this->phone_handoff_duration_ms_ / 1000.0f);
  ESP_LOGCONFIG(TAG, "  Pairing name: %s", this->pairing_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Pairing data: %s", pairing_data_valid(this->pairing_data_) ? "confirmed" : "not confirmed");
  ESP_LOGCONFIG(TAG, "  Handshake characteristic: %s",
                this->handshake_target_ == CharacteristicTarget::SESSION ? "session" : "command");
  ESP_LOGCONFIG(TAG, "  Pairing characteristic: %s",
                this->pairing_target_ == CharacteristicTarget::SESSION ? "session" : "command");
}

void WeberConnectBLE::set_probe_temperature_sensor(uint8_t probe, sensor::Sensor *sensor) {
  if (probe >= 1 && probe <= MAX_PROBES)
    this->temperature_sensors_[probe - 1] = sensor;
}

void WeberConnectBLE::set_probe_battery_sensor(uint8_t probe, sensor::Sensor *sensor) {
  if (probe >= 1 && probe <= MAX_PROBES)
    this->battery_sensors_[probe - 1] = sensor;
}

void WeberConnectBLE::set_probe_state_sensor(uint8_t probe, text_sensor::TextSensor *sensor) {
  if (probe >= 1 && probe <= MAX_PROBES)
    this->state_sensors_[probe - 1] = sensor;
}

bool WeberConnectBLE::deadline_reached_(uint32_t deadline) const {
  return static_cast<int32_t>(millis() - deadline) >= 0;
}

void WeberConnectBLE::loop() {
  const uint32_t now = millis();

  if (this->state_ == WeberState::PHONE_HANDOFF) {
    if (this->deadline_reached_(this->handoff_until_)) {
      ESP_LOGI(TAG, "Phone handoff window ended; resuming cyclic reads");
      this->next_cycle_at_ = now;
      this->set_state_(WeberState::SCAN);
    } else {
      return;
    }
  }

  if ((this->state_ == WeberState::PAIRING || this->state_ == WeberState::LISTEN ||
       this->state_ == WeberState::HANDSHAKE) &&
      this->response_handle_ != 0 && this->deadline_reached_(this->next_response_poll_at_)) {
    this->poll_response_();
    this->next_response_poll_at_ = now + 1000;
  }

  if (this->state_ == WeberState::SUBSCRIBE && now - this->state_started_at_ >= 3000) {
    ESP_LOGW(TAG, "Notification subscription setup timed out (%u/%u); continuing",
             this->subscriptions_ready_, this->subscriptions_expected_);
    this->claim_session_();
  }

  if (this->state_ == WeberState::PAIRING && now - this->state_started_at_ >= 90000) {
    this->set_error_("Pairing timed out; wake the hub and press its button after it beeps");
    this->disconnect_(true);
    return;
  }

  if (this->state_ == WeberState::LISTEN && this->deadline_reached_(this->listen_until_)) {
    if (!this->status_received_)
      ESP_LOGW(TAG, "Listen window ended without a valid INCOMING_STATUS frame");
    this->disconnect_(true);
    return;
  }

  const bool idle = this->node_state == esp32_ble_tracker::ClientState::IDLE ||
                    this->node_state == esp32_ble_tracker::ClientState::INIT;
  if (idle && this->deadline_reached_(this->next_cycle_at_)) {
    if (!pairing_data_valid(this->pairing_data_) && !this->pair_requested_) {
      this->set_state_(WeberState::ERROR);
      this->next_cycle_at_ = now + this->poll_interval_ms_;
      return;
    }
    this->reset_connection_state_();
    this->set_state_(WeberState::SCAN);
    if (!this->parent()->enabled)
      this->parent()->set_enabled(true);
  }
}

void WeberConnectBLE::request_pairing() {
  ESP_LOGI(TAG, "Pairing requested; wake the hub and press its physical button when it beeps");
  if (!pairing_material_valid(this->pairing_data_)) {
    if (!generate_pairing_data(&this->pairing_data_)) {
      this->set_error_("Could not generate P-256 pairing material");
      return;
    }
    if (!this->pairing_preference_.save(&this->pairing_data_))
      ESP_LOGW(TAG, "Could not persist pending pairing material");
  }
  this->pairing_data_.confirmed = 0;
  this->pair_requested_ = true;
  this->pairing_request_sent_ = false;
  this->version_retry_used_ = false;
  this->next_cycle_at_ = millis();
  if (this->node_state == esp32_ble_tracker::ClientState::ESTABLISHED)
    this->disconnect_(false);
  else
    this->parent()->set_enabled(true);
  this->set_state_(WeberState::SCAN);
}

void WeberConnectBLE::request_disconnect() {
  ESP_LOGI(TAG, "Manual disconnect requested");
  this->disconnect_(true);
}

void WeberConnectBLE::request_phone_handoff() {
  ESP_LOGI(TAG, "Releasing hub for the Weber phone app for %.0f seconds", this->phone_handoff_duration_ms_ / 1000.0f);
  this->pair_requested_ = false;
  this->handoff_until_ = millis() + this->phone_handoff_duration_ms_;
  this->disconnect_(false);
  this->set_state_(WeberState::PHONE_HANDOFF);
}

void WeberConnectBLE::clear_pairing_data() {
  ESP_LOGW(TAG, "Clearing local Weber pairing data");
  this->pair_requested_ = false;
  clear_pairing_data(&this->pairing_data_);
  if (!this->pairing_preference_.save(&this->pairing_data_))
    ESP_LOGW(TAG, "Could not clear pairing data in preferences");
  this->publish_hub_device_id_();
  this->disconnect_(false);
  this->set_state_(WeberState::ERROR);
}

void WeberConnectBLE::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                           esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_CONNECT_EVT:
      this->set_state_(WeberState::CONNECT);
      break;
    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK) {
        this->negotiated_mtu_ = param->cfg_mtu.mtu;
        this->mtu_received_ = true;
        if (this->state_ == WeberState::SUBSCRIBE && this->subscriptions_expected_ != 0 &&
            this->subscriptions_ready_ >= this->subscriptions_expected_)
          this->claim_session_();
      }
      break;
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK)
        this->set_state_(WeberState::DISCOVER);
      else
        this->set_error_("BLE connection failed");
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      if (!pairing_data_valid(this->pairing_data_) && !this->pair_requested_) {
        this->set_error_("Hub is not paired; press the Pair button first");
        this->disconnect_(false);
        break;
      }
      if (!this->discover_handles_()) {
        this->set_error_("Required Weber GATT service or characteristics not found");
        this->disconnect_(true);
        break;
      }
      this->subscribe_characteristics_(gattc_if);
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      if (this->state_ == WeberState::SUBSCRIBE && param->write.status == ESP_GATT_OK) {
        this->subscriptions_ready_++;
        if (this->subscriptions_ready_ >= this->subscriptions_expected_ && this->mtu_received_)
          this->claim_session_();
      }
      break;
    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "GATT write failed on handle 0x%04x, status=%d", param->write.handle, param->write.status);
        break;
      }
      if (this->state_ == WeberState::CLAIM && param->write.handle == this->session_handle_)
        this->send_handshake_();
      break;
    case ESP_GATTC_NOTIFY_EVT:
      this->process_frame_(param->notify.value, param->notify.value_len, "notify");
      break;
    case ESP_GATTC_READ_CHAR_EVT:
      if (param->read.status == ESP_GATT_OK && param->read.handle == this->response_handle_)
        this->process_frame_(param->read.value, param->read.value_len, "response-read");
      break;
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGD(TAG, "BLE disconnected, reason=0x%02x", param->disconnect.reason);
      this->reset_connection_state_();
      if (this->state_ != WeberState::PHONE_HANDOFF && this->state_ != WeberState::ERROR) {
        this->next_cycle_at_ = millis() + (this->disconnect_schedule_next_ ? this->poll_interval_ms_ : 500U);
        this->set_state_(WeberState::DISCONNECT);
      }
      break;
    default:
      break;
  }
}

bool WeberConnectBLE::discover_handles_() {
  if (this->parent()->get_service(APPLIANCE_SERVICE_UUID) == nullptr)
    return false;
  auto find_handle = [this](const esp32_ble_tracker::ESPBTUUID &uuid) -> uint16_t {
    auto *characteristic = this->parent()->get_characteristic(APPLIANCE_SERVICE_UUID, uuid);
    return characteristic == nullptr ? 0 : characteristic->handle;
  };
  this->status_handle_ = find_handle(STATUS_UUID);
  this->notification_handle_ = find_handle(NOTIFICATION_UUID);
  this->command_handle_ = find_handle(COMMAND_UUID);
  this->session_handle_ = find_handle(SESSION_UUID);
  this->response_handle_ = find_handle(RESPONSE_UUID);
  ESP_LOGD(TAG, "Handles status=%04x notification=%04x command=%04x session=%04x response=%04x",
           this->status_handle_, this->notification_handle_, this->command_handle_, this->session_handle_,
           this->response_handle_);
  return this->status_handle_ != 0 && this->notification_handle_ != 0 && this->command_handle_ != 0 &&
         this->session_handle_ != 0 && this->response_handle_ != 0;
}

void WeberConnectBLE::subscribe_characteristics_(esp_gatt_if_t gattc_if) {
  this->set_state_(WeberState::SUBSCRIBE);
  this->subscriptions_expected_ = 0;
  this->subscriptions_ready_ = 0;
  const uint16_t handles[] = {this->status_handle_, this->notification_handle_, this->response_handle_};
  for (uint16_t handle : handles) {
    const esp_err_t result = esp_ble_gattc_register_for_notify(gattc_if, this->parent()->get_remote_bda(), handle);
    if (result == ESP_OK)
      this->subscriptions_expected_++;
    else
      ESP_LOGW(TAG, "Could not register notifications for handle 0x%04x: %d", handle, result);
  }
  if (this->subscriptions_expected_ == 0) {
    this->set_error_("No Weber notification characteristic could be subscribed");
    this->disconnect_(true);
  }
}

void WeberConnectBLE::claim_session_() {
  if (this->state_ != WeberState::SUBSCRIBE)
    return;
  if (this->negotiated_mtu_ < REQUIRED_MINIMUM_MTU) {
    ESP_LOGE(TAG, "Negotiated ATT MTU %u is below required minimum %u", this->negotiated_mtu_,
             REQUIRED_MINIMUM_MTU);
    this->set_error_("ATT MTU is too small for unfragmented Saber frames");
    this->disconnect_(true);
    return;
  }
  this->set_state_(WeberState::CLAIM);
  const uint8_t claim = 0x01;
  if (!this->write_value_(this->session_handle_, &claim, sizeof(claim), ESP_GATT_WRITE_TYPE_RSP)) {
    this->set_error_("Could not claim Weber session slot");
    this->disconnect_(true);
  }
}

void WeberConnectBLE::send_handshake_() {
  std::array<uint8_t, 32> nonce{};
  if (!fill_random(nonce.data(), nonce.size())) {
    this->set_error_("Could not generate handshake nonce");
    this->disconnect_(true);
    return;
  }
  const auto frame = build_handshake_frame(this->sequence_++, this->message_version_, this->pairing_data_.companion_id,
                                           nonce);
  const uint16_t handle = this->pair_requested_ ? this->command_handle_ : this->target_handle_(this->handshake_target_);
  this->set_state_(WeberState::HANDSHAKE);
  if (!this->write_frame_(handle, frame)) {
    this->set_error_("Could not write Weber handshake");
    this->disconnect_(true);
    return;
  }
  this->next_response_poll_at_ = millis() + 250;
  if (this->pair_requested_) {
    this->set_state_(WeberState::PAIRING);
  } else {
    this->status_received_ = false;
    this->listen_until_ = millis() + this->listen_duration_ms_;
    this->set_state_(WeberState::LISTEN);
  }
}

void WeberConnectBLE::send_pairing_request_() {
  if (this->pairing_request_sent_)
    return;
  const auto frame = build_pairing_frame(this->sequence_++, this->message_version_, this->pairing_data_,
                                         this->pairing_name_);
  if (frame.empty()) {
    this->set_error_("Could not build pairing frame");
    this->disconnect_(true);
    return;
  }
  if (frame.size() + 3U > this->negotiated_mtu_) {
    ESP_LOGE(TAG, "Pairing frame requires ATT MTU %u, negotiated %u", static_cast<unsigned>(frame.size() + 3U),
             this->negotiated_mtu_);
    this->set_error_("ATT MTU is too small for the configured pairing name");
    this->disconnect_(true);
    return;
  }
  if (this->write_frame_(this->target_handle_(this->pairing_target_), frame)) {
    this->pairing_request_sent_ = true;
    this->state_started_at_ = millis();
    ESP_LOGI(TAG, "Pairing request sent; press the physical button on the hub when it beeps");
  } else {
    this->set_error_("Could not write Weber pairing request");
    this->disconnect_(true);
  }
}

bool WeberConnectBLE::write_value_(uint16_t handle, const uint8_t *data, size_t length,
                                   esp_gatt_write_type_t write_type) {
  if (handle == 0 || data == nullptr || length == 0 || length > UINT16_MAX)
    return false;
  const esp_err_t result = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle, static_cast<uint16_t>(length),
      const_cast<uint8_t *>(data), write_type, ESP_GATT_AUTH_REQ_NONE);
  if (result != ESP_OK)
    ESP_LOGW(TAG, "esp_ble_gattc_write_char failed for handle 0x%04x: %d", handle, result);
  return result == ESP_OK;
}

bool WeberConnectBLE::write_frame_(uint16_t handle, const std::vector<uint8_t> &frame) {
  if (frame.empty())
    return false;
  if (frame.size() + 3U > this->negotiated_mtu_) {
    ESP_LOGE(TAG, "Saber frame length %u requires ATT MTU %u, negotiated %u", static_cast<unsigned>(frame.size()),
             static_cast<unsigned>(frame.size() + 3U), this->negotiated_mtu_);
    return false;
  }
  ESP_LOGV(TAG, "Writing Saber frame (%u bytes) to handle 0x%04x", static_cast<unsigned>(frame.size()), handle);
  return this->write_value_(handle, frame.data(), frame.size(), ESP_GATT_WRITE_TYPE_RSP);
}

void WeberConnectBLE::poll_response_() {
  if (this->response_handle_ == 0 || this->parent()->get_conn_id() == esp32_ble_client::UNSET_CONN_ID)
    return;
  const esp_err_t result = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                                   this->response_handle_, ESP_GATT_AUTH_REQ_NONE);
  if (result != ESP_OK)
    ESP_LOGV(TAG, "Response read request failed: %d", result);
}

void WeberConnectBLE::process_frame_(const uint8_t *data, size_t length, const char *source) {
  SaberFrameView frame;
  if (!parse_saber_frame(data, length, &frame)) {
    ESP_LOGV(TAG, "Ignoring invalid Saber frame from %s (%u bytes)", source, static_cast<unsigned>(length));
    return;
  }
  ESP_LOGD(TAG, "Saber frame source=%s sequence=%u version=%u type=0x%02x payload=%u", source, frame.sequence,
           frame.message_version, frame.message_type, static_cast<unsigned>(frame.payload_length));
  switch (frame.message_type) {
    case MSG_INCOMING_STATUS:
      this->process_status_(frame);
      break;
    case MSG_INCOMING_PAIRING_REQUIRED:
      if (this->pair_requested_)
        this->send_pairing_request_();
      break;
    case MSG_INCOMING_PAIRING_RESPONSE:
      this->process_pairing_response_(frame);
      break;
    case MSG_INCOMING_HANDSHAKE_SUCCESS:
      ESP_LOGD(TAG, "Hub accepted handshake");
      break;
    case MSG_INCOMING_ERROR:
      if (frame.payload_length >= 3 && frame.payload[0] == 0 && frame.payload[1] >= 1 && frame.payload[2] == 0 &&
          !this->version_retry_used_) {
        this->message_version_ = frame.message_version;
        this->version_retry_used_ = true;
        ESP_LOGW(TAG, "Hub rejected the message version; retrying handshake at version %u", this->message_version_);
        this->send_handshake_();
      } else {
        ESP_LOGW(TAG, "Hub returned INCOMING_ERROR (version %u)", frame.message_version);
      }
      break;
    default:
      break;
  }
}

void WeberConnectBLE::process_status_(const SaberFrameView &frame) {
  CookStatus status;
  if (!parse_cook_status(frame.payload, frame.payload_length, &status)) {
    ESP_LOGW(TAG, "Rejected malformed INCOMING_STATUS TLV payload");
    return;
  }
  this->status_received_ = true;
  this->publish_probe_status_(status);
}

void WeberConnectBLE::process_pairing_response_(const SaberFrameView &frame) {
  if (!this->pair_requested_ || frame.payload == nullptr || frame.payload_length < 81) {
    ESP_LOGW(TAG, "Ignoring unexpected or short pairing response");
    return;
  }
  const uint8_t status = frame.payload[80];
  if (status == 0) {
    std::copy(frame.payload, frame.payload + 16, this->pairing_data_.appliance_id.begin());
    std::copy(frame.payload + 16, frame.payload + 80, this->pairing_data_.appliance_public_key.begin());
    this->pairing_data_.confirmed = 1;
    if (!this->pairing_preference_.save(&this->pairing_data_)) {
      this->set_error_("Pairing succeeded but could not be saved to NVS");
      this->disconnect_(true);
      return;
    }
    ESP_LOGI(TAG, "Pairing confirmed and saved");
    this->pair_requested_ = false;
    this->publish_hub_device_id_();
    this->disconnect_(true);
  } else if (status == 1) {
    this->set_error_("Hub rejected pairing");
    this->disconnect_(true);
  } else if (status == 2) {
    this->set_error_("Hub pairing confirmation timed out");
    this->disconnect_(true);
  } else {
    this->set_error_("Hub returned an unknown pairing status");
    this->disconnect_(true);
  }
}

void WeberConnectBLE::publish_probe_status_(const CookStatus &status) {
  for (size_t index = 0; index < MAX_PROBES; index++) {
    const ProbeStatus &probe = status.probes[index];
    if (this->temperature_sensors_[index] != nullptr) {
      this->temperature_sensors_[index]->publish_state(
          probe.present && probe.temperature_valid ? probe.temperature_deci_c / 10.0f : NAN);
    }
    if (this->battery_sensors_[index] != nullptr) {
      this->battery_sensors_[index]->publish_state(probe.present && probe.battery_valid ? probe.battery_percent : NAN);
    }
    if (this->state_sensors_[index] != nullptr) {
      std::string state = probe.present ? probe_state_name(probe.state) : "No probe";
      if (probe.present) {
        state += " (";
        state += probe_type_name(probe.probe_type);
        state += ")";
      }
      this->state_sensors_[index]->publish_state(state);
    }
  }
  ESP_LOGI(TAG, "Published status for %u probe(s)", static_cast<unsigned>(status.probe_count));
}

void WeberConnectBLE::publish_hub_device_id_() {
  if (this->hub_device_id_sensor_ == nullptr)
    return;
  if (!pairing_data_valid(this->pairing_data_)) {
    this->hub_device_id_sensor_->publish_state("Not paired");
    return;
  }
  char output[33];
  for (size_t index = 0; index < this->pairing_data_.appliance_id.size(); index++)
    snprintf(output + index * 2, sizeof(output) - index * 2, "%02x", this->pairing_data_.appliance_id[index]);
  output[32] = '\0';
  this->hub_device_id_sensor_->publish_state(output);
}

void WeberConnectBLE::disconnect_(bool schedule_next) {
  const bool preserve_state = this->state_ == WeberState::ERROR || this->state_ == WeberState::PHONE_HANDOFF;
  this->disconnect_schedule_next_ = schedule_next;
  if (schedule_next)
    this->next_cycle_at_ = millis() + this->poll_interval_ms_;
  if (this->parent()->enabled)
    this->parent()->set_enabled(false);
  if (!preserve_state)
    this->set_state_(WeberState::DISCONNECT);
}

void WeberConnectBLE::reset_connection_state_() {
  this->status_handle_ = 0;
  this->notification_handle_ = 0;
  this->command_handle_ = 0;
  this->session_handle_ = 0;
  this->response_handle_ = 0;
  this->subscriptions_expected_ = 0;
  this->subscriptions_ready_ = 0;
  this->negotiated_mtu_ = 23;
  this->mtu_received_ = false;
  this->status_received_ = false;
  this->pairing_request_sent_ = false;
}

uint16_t WeberConnectBLE::target_handle_(CharacteristicTarget target) const {
  return target == CharacteristicTarget::SESSION ? this->session_handle_ : this->command_handle_;
}

void WeberConnectBLE::set_state_(WeberState state) {
  if (this->state_ == state)
    return;
  this->state_ = state;
  this->state_started_at_ = millis();
  ESP_LOGD(TAG, "State -> %s", this->state_name_(state));
  if (this->connection_state_sensor_ != nullptr)
    this->connection_state_sensor_->publish_state(this->state_name_(state));
}

void WeberConnectBLE::set_error_(const char *message) {
  ESP_LOGE(TAG, "%s", message);
  this->set_state_(WeberState::ERROR);
  this->next_cycle_at_ = millis() + this->poll_interval_ms_;
}

const char *WeberConnectBLE::state_name_(WeberState state) const {
  switch (state) {
    case WeberState::SCAN:
      return "SCAN";
    case WeberState::CONNECT:
      return "CONNECT";
    case WeberState::DISCOVER:
      return "DISCOVER";
    case WeberState::SUBSCRIBE:
      return "SUBSCRIBE";
    case WeberState::CLAIM:
      return "CLAIM";
    case WeberState::HANDSHAKE:
      return "HANDSHAKE";
    case WeberState::PAIRING:
      return "PAIRING";
    case WeberState::LISTEN:
      return "LISTEN";
    case WeberState::DISCONNECT:
      return "DISCONNECT";
    case WeberState::ERROR:
      return "ERROR";
    case WeberState::PHONE_HANDOFF:
      return "PHONE_HANDOFF";
  }
  return "UNKNOWN";
}

}  // namespace esphome::weber_connect_ble

#endif  // USE_ESP32
