#include "esp32_improv_thread_component.h"

#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble_server/ble_2902.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32

namespace esphome {
namespace esp32_improv_thread {

using namespace bytebuffer;

static const char *const TAG = "esp32_improv_thread.component";
static const char *const ESPHOME_MY_LINK = "https://my.home-assistant.io/redirect/config_flow_start?domain=esphome";
static constexpr uint16_t STOP_ADVERTISING_DELAY =
    10000;  // Delay (ms) before stopping service to allow BLE clients to read the final state
static constexpr uint16_t NAME_ADVERTISING_INTERVAL = 60000;  // Advertise name every 60 seconds
static constexpr uint16_t NAME_ADVERTISING_DURATION = 1000;   // Advertise name for 1 second

// Improv Thread service data constants
static constexpr uint8_t IMPROV_SERVICE_DATA_SIZE = 8;
static constexpr uint8_t IMPROV_PROTOCOL_ID_1 = 0x54;  // 'T'
static constexpr uint8_t IMPROV_PROTOCOL_ID_2 = 0x48;  // 'H'

// Thread-specific RPC command
static constexpr uint8_t THREAD_SETTINGS = 0x01;

ESP32ImprovThreadComponent::ESP32ImprovThreadComponent() { global_improv_thread_component = this; }

void ESP32ImprovThreadComponent::setup() {
#ifdef USE_BINARY_SENSOR
  if (this->authorizer_ != nullptr) {
    this->authorizer_->add_on_state_callback([this](bool state) {
      if (state) {
        this->authorized_start_ = millis();
        this->identify_start_ = 0;
      }
    });
  }
#endif
  global_ble_server->on_disconnect([this](uint16_t conn_id) { this->set_error_(improv::ERROR_NONE); });

  // Start with loop disabled - will be enabled by start() when needed
  this->disable_loop();
}

void ESP32ImprovThreadComponent::setup_characteristics() {
  this->status_ = this->service_->create_characteristic(
      improv::STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor *status_descriptor = new BLE2902();
  this->status_->add_descriptor(status_descriptor);

  this->error_ = this->service_->create_characteristic(
      improv::ERROR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor *error_descriptor = new BLE2902();
  this->error_->add_descriptor(error_descriptor);

  this->rpc_ = this->service_->create_characteristic(improv::RPC_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
  this->rpc_->on_write([this](std::span<const uint8_t> data, uint16_t id) {
    if (!data.empty()) {
      this->incoming_data_.insert(this->incoming_data_.end(), data.begin(), data.end());
    }
  });
  BLEDescriptor *rpc_descriptor = new BLE2902();
  this->rpc_->add_descriptor(rpc_descriptor);

  this->rpc_response_ = this->service_->create_characteristic(
      improv::RPC_RESULT_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor *rpc_response_descriptor = new BLE2902();
  this->rpc_response_->add_descriptor(rpc_response_descriptor);

  this->capabilities_ =
      this->service_->create_characteristic(improv::CAPABILITIES_UUID, BLECharacteristic::PROPERTY_READ);
  BLEDescriptor *capabilities_descriptor = new BLE2902();
  this->capabilities_->add_descriptor(capabilities_descriptor);
  uint8_t capabilities = 0x00;
#ifdef USE_OUTPUT
  if (this->status_indicator_ != nullptr)
    capabilities |= improv::CAPABILITY_IDENTIFY;
#endif
  this->capabilities_->set_value(ByteBuffer::wrap(capabilities));
  this->setup_complete_ = true;
}

void ESP32ImprovThreadComponent::loop() {
  if (!global_ble_server->is_running()) {
    if (this->state_ != improv::STATE_STOPPED) {
      this->state_ = improv::STATE_STOPPED;
#ifdef USE_ESP32_IMPROV_THREAD_STATE_CALLBACK
      this->state_callback_.call(this->state_, this->error_state_);
#endif
    }
    this->incoming_data_.clear();
    return;
  }
  if (this->service_ == nullptr) {
    // Setup the service
    ESP_LOGD(TAG, "Creating Improv Thread service");
    this->service_ = global_ble_server->create_service(ESPBTUUID::from_raw(improv::SERVICE_UUID), true);
    this->setup_characteristics();
  }

  if (!this->incoming_data_.empty())
    this->process_incoming_data_();
  uint32_t now = App.get_loop_component_start_time();

  // Check if we need to update advertising type
  if (this->state_ != improv::STATE_STOPPED && this->state_ != improv::STATE_PROVISIONED) {
    this->update_advertising_type_();
  }

  switch (this->state_) {
    case improv::STATE_STOPPED:
      this->set_status_indicator_state_(false);

      if (this->should_start_ && this->setup_complete_) {
        if (this->service_->is_created()) {
          this->service_->start();
        } else if (this->service_->is_running()) {
          // Start by advertising the device name first BEFORE setting any state
          ESP_LOGV(TAG, "Starting with device name advertising");
          this->advertising_device_name_ = true;
          this->last_name_adv_time_ = App.get_loop_component_start_time();
          esp32_ble::global_ble->advertising_set_service_data_and_name(std::span<const uint8_t>{}, true);
          esp32_ble::global_ble->advertising_start();

          // Set initial state based on whether we have an authorizer
          this->set_state_(this->get_initial_state_(), false);
          this->set_error_(improv::ERROR_NONE);
          this->should_start_ = false;  // Clear flag after starting
          ESP_LOGD(TAG, "Service started!");
        }
      }
      break;
    case improv::STATE_AWAITING_AUTHORIZATION: {
#ifdef USE_BINARY_SENSOR
      if (this->authorizer_ == nullptr ||
          (this->authorized_start_ != 0 && ((now - this->authorized_start_) < this->authorized_duration_))) {
        this->set_state_(improv::STATE_AUTHORIZED);
      } else {
        if (!this->check_identify_())
          this->set_status_indicator_state_(true);
      }
#else
      this->set_state_(improv::STATE_AUTHORIZED);
#endif
      this->check_thread_connection_();
      break;
    }
    case improv::STATE_AUTHORIZED: {
#ifdef USE_BINARY_SENSOR
      if (this->authorizer_ != nullptr && now - this->authorized_start_ > this->authorized_duration_) {
        ESP_LOGD(TAG, "Authorization timeout");
        this->set_state_(improv::STATE_AWAITING_AUTHORIZATION);
        return;
      }
#endif
      if (!this->check_identify_()) {
        this->set_status_indicator_state_((now % 1000) < 500);
      }
      this->check_thread_connection_();
      break;
    }
    case improv::STATE_PROVISIONING: {
      this->set_status_indicator_state_((now % 200) < 100);
      this->check_thread_connection_();
      break;
    }
    case improv::STATE_PROVISIONED: {
      this->incoming_data_.clear();
      this->set_status_indicator_state_(false);
      // Provisioning complete, no further loop execution needed
      this->disable_loop();
      break;
    }
  }
}

void ESP32ImprovThreadComponent::set_status_indicator_state_(bool state) {
#ifdef USE_OUTPUT
  if (this->status_indicator_ == nullptr)
    return;
  if (this->status_indicator_state_ == state)
    return;
  this->status_indicator_state_ = state;
  if (state) {
    this->status_indicator_->turn_on();
  } else {
    this->status_indicator_->turn_off();
  }
#endif
}

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
const char *ESP32ImprovThreadComponent::state_to_string_(improv::State state) {
  switch (state) {
    case improv::STATE_STOPPED:
      return "STOPPED";
    case improv::STATE_AWAITING_AUTHORIZATION:
      return "AWAITING_AUTHORIZATION";
    case improv::STATE_AUTHORIZED:
      return "AUTHORIZED";
    case improv::STATE_PROVISIONING:
      return "PROVISIONING";
    case improv::STATE_PROVISIONED:
      return "PROVISIONED";
    default:
      return "UNKNOWN";
  }
}
#endif

bool ESP32ImprovThreadComponent::check_identify_() {
  uint32_t now = millis();

  bool identify = this->identify_start_ != 0 && now - this->identify_start_ <= this->identify_duration_;

  if (identify) {
    uint32_t time = now % 1000;
    this->set_status_indicator_state_(time < 600 && time % 200 < 100);
  }
  return identify;
}

void ESP32ImprovThreadComponent::set_state_(improv::State state, bool update_advertising) {
  // Skip if state hasn't changed
  if (this->state_ == state) {
    return;
  }

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  ESP_LOGD(TAG, "State transition: %s (0x%02X) -> %s (0x%02X)", this->state_to_string_(this->state_), this->state_,
           this->state_to_string_(state), state);
#endif
  this->state_ = state;
  if (this->status_ != nullptr && (this->status_->get_value().empty() || this->status_->get_value()[0] != state)) {
    this->status_->set_value(ByteBuffer::wrap(static_cast<uint8_t>(state)));
    if (state != improv::STATE_STOPPED)
      this->status_->notify();
  }
  // Only advertise valid Improv states (0x01-0x04).
  // STATE_STOPPED (0x00) is internal only and not part of the Improv spec.
  if (state != improv::STATE_STOPPED && update_advertising) {
    // State change always overrides name advertising and resets the timer
    this->advertising_device_name_ = false;
    // Reset the timer so we wait another 60 seconds before advertising name
    this->last_name_adv_time_ = App.get_loop_component_start_time();
    // Advertise the new state via service data
    this->advertise_service_data_();
  }
#ifdef USE_ESP32_IMPROV_THREAD_STATE_CALLBACK
  this->state_callback_.call(this->state_, this->error_state_);
#endif
}

void ESP32ImprovThreadComponent::set_error_(improv::Error error) {
  if (error != improv::ERROR_NONE) {
    ESP_LOGE(TAG, "Error: %d", error);
  }
  if (this->error_ != nullptr && (this->error_->get_value().empty() || this->error_->get_value()[0] != error)) {
    this->error_->set_value(ByteBuffer::wrap(static_cast<uint8_t>(error)));
    if (this->state_ != improv::STATE_STOPPED)
      this->error_->notify();
  }
}

void ESP32ImprovThreadComponent::send_response_(std::vector<uint8_t> &&response) {
  this->rpc_response_->set_value(std::move(response));
  if (this->state_ != improv::STATE_STOPPED)
    this->rpc_response_->notify();
}

void ESP32ImprovThreadComponent::start() {
  if (this->should_start_ || this->state_ != improv::STATE_STOPPED)
    return;

  ESP_LOGD(TAG, "Setting Improv Thread to start");
  this->should_start_ = true;
  this->enable_loop();
}

void ESP32ImprovThreadComponent::stop() {
  this->should_start_ = false;
  // Wait before stopping the service to ensure all BLE clients see the state change.
  this->set_timeout("end-service", STOP_ADVERTISING_DELAY, [this] {
    if (this->state_ == improv::STATE_STOPPED || this->service_ == nullptr)
      return;
    this->service_->stop();
    this->set_state_(improv::STATE_STOPPED);
  });
}

float ESP32ImprovThreadComponent::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH; }

void ESP32ImprovThreadComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP32 Improv Thread:");
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Authorizer", this->authorizer_);
#endif
#ifdef USE_OUTPUT
  ESP_LOGCONFIG(TAG, "  Status Indicator: '%s'", YESNO(this->status_indicator_ != nullptr));
#endif
}

void ESP32ImprovThreadComponent::process_incoming_data_() {
  uint8_t length = this->incoming_data_[1];

  ESP_LOGV(TAG, "Processing bytes - %s", format_hex_pretty(this->incoming_data_).c_str());
  if (this->incoming_data_.size() - 3 == length) {
    this->set_error_(improv::ERROR_NONE);
    improv::ImprovCommand command = improv::parse_improv_data(this->incoming_data_);
    switch (command.command) {
      case improv::BAD_CHECKSUM:
        ESP_LOGW(TAG, "Error decoding Improv payload");
        this->set_error_(improv::ERROR_INVALID_RPC);
        this->incoming_data_.clear();
        break;
      case THREAD_SETTINGS: {
        if (this->state_ != improv::STATE_AUTHORIZED) {
          ESP_LOGW(TAG, "Settings received, but not authorized");
          this->set_error_(improv::ERROR_NOT_AUTHORIZED);
          this->incoming_data_.clear();
          return;
        }

        // Parse Thread dataset from command
        // The dataset is expected to be in TLV format as a hex string or raw bytes
        if (command.ssid.empty()) {
          ESP_LOGW(TAG, "Empty Thread dataset received");
          this->set_error_(improv::ERROR_INVALID_RPC);
          this->incoming_data_.clear();
          return;
        }

        // Validate hex string format
        if (command.ssid.length() % 2 != 0) {
          ESP_LOGW(TAG, "Invalid hex string length (must be even)");
          this->set_error_(improv::ERROR_INVALID_RPC);
          this->incoming_data_.clear();
          return;
        }

        // Convert hex string to bytes
        std::vector<uint8_t> dataset_bytes;
        for (size_t i = 0; i + 1 < command.ssid.length(); i += 2) {
          std::string byte_string = command.ssid.substr(i, 2);
          // Validate we got 2 characters and both are hex
          if (byte_string.length() != 2 || !std::isxdigit(byte_string[0]) || !std::isxdigit(byte_string[1])) {
            ESP_LOGW(TAG, "Invalid hex character in dataset");
            this->set_error_(improv::ERROR_INVALID_RPC);
            this->incoming_data_.clear();
            return;
          }
          uint8_t byte = (uint8_t) strtol(byte_string.c_str(), nullptr, 16);
          dataset_bytes.push_back(byte);
        }

        this->dataset_tlv_ = dataset_bytes;

        // Set Thread dataset
        if (openthread::global_openthread_component == nullptr) {
          ESP_LOGE(TAG, "OpenThread component not available");
          this->set_error_(improv::ERROR_UNKNOWN_RPC);
          this->incoming_data_.clear();
          return;
        }

        if (!openthread::global_openthread_component->set_dataset_tlv(dataset_bytes)) {
          ESP_LOGE(TAG, "Failed to set Thread dataset");
          this->set_error_(improv::ERROR_INVALID_RPC);
          this->incoming_data_.clear();
          return;
        }

        // Start joining Thread network
        if (!openthread::global_openthread_component->start_joining()) {
          ESP_LOGE(TAG, "Failed to start Thread joining");
          this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
          this->incoming_data_.clear();
          return;
        }

        this->set_state_(improv::STATE_PROVISIONING);
        ESP_LOGD(TAG, "Received Improv Thread dataset, joining network");

        auto f = std::bind(&ESP32ImprovThreadComponent::on_thread_connect_timeout_, this);
        this->set_timeout("thread-connect-timeout", this->thread_timeout_, f);
        this->incoming_data_.clear();
        break;
      }
      case improv::IDENTIFY:
        this->incoming_data_.clear();
        this->identify_start_ = millis();
        break;
      default:
        ESP_LOGW(TAG, "Unknown Improv payload");
        this->set_error_(improv::ERROR_UNKNOWN_RPC);
        this->incoming_data_.clear();
    }
  } else if (this->incoming_data_.size() - 2 > length) {
    ESP_LOGV(TAG, "Too much data received or data malformed; resetting buffer");
    this->incoming_data_.clear();
  } else {
    ESP_LOGV(TAG, "Waiting for split data packets");
  }
}

void ESP32ImprovThreadComponent::on_thread_connect_timeout_() {
  this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
  this->set_state_(improv::STATE_AUTHORIZED);
#ifdef USE_BINARY_SENSOR
  if (this->authorizer_ != nullptr)
    this->authorized_start_ = millis();
#endif
  ESP_LOGW(TAG, "Timed out while joining Thread network");
  if (openthread::global_openthread_component != nullptr) {
    openthread::global_openthread_component->clear_dataset();
  }
}

void ESP32ImprovThreadComponent::check_thread_connection_() {
  if (openthread::global_openthread_component == nullptr) {
    return;
  }

  if (!openthread::global_openthread_component->is_joined()) {
    return;
  }

  if (this->state_ == improv::STATE_PROVISIONING) {
    // Save the dataset
    openthread::global_openthread_component->save_dataset();
    this->cancel_timeout("thread-connect-timeout");

    // Build URL list with minimal allocations
    std::string url_strings[3];
    size_t url_count = 0;

#ifdef USE_ESP32_IMPROV_THREAD_NEXT_URL
    // Add next_url if configured (should be first per Improv BLE spec)
    std::string next_url = this->get_formatted_next_url_();
    if (!next_url.empty()) {
      url_strings[url_count++] = std::move(next_url);
    }
#endif

    // Add default URLs
    url_strings[url_count++] = ESPHOME_MY_LINK;

    // Add IPv6 address if available
    auto omr_addr = openthread::global_openthread_component->get_omr_address();
    if (omr_addr.has_value()) {
      std::string addr_str = improv_base::ImprovBase::format_ipv6_address(*omr_addr);
#ifdef USE_WEBSERVER
      // Use string concatenation for dynamic URL construction
      std::string webserver_url = "http://[" + addr_str + "]:" + to_string(USE_WEBSERVER_PORT);
      if (url_count < 3) {
        url_strings[url_count++] = webserver_url;
      }
#endif
    }

    this->send_response_(improv::build_rpc_response(THREAD_SETTINGS,
                                                    std::vector<std::string>(url_strings, url_strings + url_count)));
  } else if (this->is_active() && this->state_ != improv::STATE_PROVISIONED) {
    ESP_LOGD(TAG, "Thread provisioned externally");
  }

  this->set_state_(improv::STATE_PROVISIONED);
  this->stop();
}

void ESP32ImprovThreadComponent::advertise_service_data_() {
  uint8_t service_data[IMPROV_SERVICE_DATA_SIZE] = {};
  service_data[0] = IMPROV_PROTOCOL_ID_1;  // TH
  service_data[1] = IMPROV_PROTOCOL_ID_2;
  service_data[2] = static_cast<uint8_t>(this->state_);

  uint8_t capabilities = 0x00;
#ifdef USE_OUTPUT
  if (this->status_indicator_ != nullptr)
    capabilities |= improv::CAPABILITY_IDENTIFY;
#endif

  service_data[3] = capabilities;
  // service_data[4-7] are already 0 (Reserved)

  // Atomically set service data and disable name in advertising
  esp32_ble::global_ble->advertising_set_service_data_and_name(std::span<const uint8_t>(service_data), false);
}

void ESP32ImprovThreadComponent::update_advertising_type_() {
  uint32_t now = App.get_loop_component_start_time();

  // If we're advertising the device name and it's been more than NAME_ADVERTISING_DURATION, switch back to service data
  if (this->advertising_device_name_) {
    if (now - this->last_name_adv_time_ >= NAME_ADVERTISING_DURATION) {
      ESP_LOGV(TAG, "Switching back to service data advertising");
      this->advertising_device_name_ = false;
      // Restore service data advertising
      this->advertise_service_data_();
    }
    return;
  }

  // Check if it's time to advertise the device name (every NAME_ADVERTISING_INTERVAL)
  if (now - this->last_name_adv_time_ >= NAME_ADVERTISING_INTERVAL) {
    ESP_LOGV(TAG, "Switching to device name advertising");
    this->advertising_device_name_ = true;
    this->last_name_adv_time_ = now;

    // Atomically clear service data and enable name in advertising data
    esp32_ble::global_ble->advertising_set_service_data_and_name(std::span<const uint8_t>{}, true);
  }
}

improv::State ESP32ImprovThreadComponent::get_initial_state_() const {
#ifdef USE_BINARY_SENSOR
  // If we have an authorizer, start in awaiting authorization state
  return this->authorizer_ == nullptr ? improv::STATE_AUTHORIZED : improv::STATE_AWAITING_AUTHORIZATION;
#else
  // No binary_sensor support = no authorizer possible, start as authorized
  return improv::STATE_AUTHORIZED;
#endif
}

ESP32ImprovThreadComponent *global_improv_thread_component =
    nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esp32_improv_thread
}  // namespace esphome

#endif
