#include "improv_serial_thread_component.h"
#ifdef USE_OPENTHREAD
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include "esphome/components/logger/logger.h"

namespace esphome {
namespace improv_serial_thread {

static const char *const TAG = "improv_serial_thread";

void ImprovSerialThreadComponent::setup() {
  global_improv_serial_thread_component = this;
#ifdef USE_ESP32
  this->uart_num_ = logger::global_logger->get_uart_num();
#elif defined(USE_ARDUINO)
  this->hw_serial_ = logger::global_logger->get_hw_serial();
#endif

  if (openthread::global_openthread_component != nullptr &&
      openthread::global_openthread_component->has_dataset()) {
    this->state_ = improv::STATE_PROVISIONED;
  }
}

void ImprovSerialThreadComponent::loop() {
  if (this->last_read_byte_ && (millis() - this->last_read_byte_ > IMPROV_SERIAL_TIMEOUT)) {
    this->last_read_byte_ = 0;
    this->rx_buffer_.clear();
    ESP_LOGV(TAG, "Timeout");
  }

  auto byte = this->read_byte_();
  while (byte.has_value()) {
    if (this->parse_improv_serial_byte_(byte.value())) {
      this->last_read_byte_ = millis();
    } else {
      this->last_read_byte_ = 0;
      this->rx_buffer_.clear();
    }
    byte = this->read_byte_();
  }

  if (this->state_ == improv::STATE_PROVISIONING) {
    if (openthread::global_openthread_component != nullptr &&
        openthread::global_openthread_component->is_joined()) {
      openthread::global_openthread_component->save_dataset();
      this->cancel_timeout("thread-connect-timeout");
      this->set_state_(improv::STATE_PROVISIONED);

      std::vector<uint8_t> url = this->build_rpc_settings_response_(THREAD_SETTINGS);
      this->send_response_(url);
    }
  }
}

void ImprovSerialThreadComponent::dump_config() { ESP_LOGCONFIG(TAG, "Improv Serial Thread:"); }

optional<uint8_t> ImprovSerialThreadComponent::read_byte_() {
  optional<uint8_t> byte;
  uint8_t data = 0;
#ifdef USE_ESP32
  switch (logger::global_logger->get_uart()) {
    case logger::UART_SELECTION_UART0:
    case logger::UART_SELECTION_UART1:
#if !defined(USE_ESP32_VARIANT_ESP32C3) && !defined(USE_ESP32_VARIANT_ESP32C6) && \
    !defined(USE_ESP32_VARIANT_ESP32C61) && !defined(USE_ESP32_VARIANT_ESP32S2) && !defined(USE_ESP32_VARIANT_ESP32S3)
    case logger::UART_SELECTION_UART2:
#endif  // !USE_ESP32_VARIANT_ESP32C3 && !USE_ESP32_VARIANT_ESP32C6 && !USE_ESP32_VARIANT_ESP32C61 &&
        // !USE_ESP32_VARIANT_ESP32S2 && !USE_ESP32_VARIANT_ESP32S3
      if (this->uart_num_ >= 0) {
        size_t available;
        uart_get_buffered_data_len(this->uart_num_, &available);
        if (available) {
          uart_read_bytes(this->uart_num_, &data, 1, 0);
          byte = data;
        }
      }
      break;
#if defined(USE_LOGGER_USB_CDC) && defined(CONFIG_ESP_CONSOLE_USB_CDC)
    case logger::UART_SELECTION_USB_CDC:
      if (esp_usb_console_available_for_read()) {
        esp_usb_console_read_buf((char *) &data, 1);
        byte = data;
      }
      break;
#endif  // USE_LOGGER_USB_CDC
#ifdef USE_LOGGER_USB_SERIAL_JTAG
    case logger::UART_SELECTION_USB_SERIAL_JTAG: {
      if (usb_serial_jtag_read_bytes((char *) &data, 1, 0)) {
        byte = data;
      }
      break;
    }
#endif  // USE_LOGGER_USB_SERIAL_JTAG
    default:
      break;
  }
#elif defined(USE_ARDUINO)
  if (this->hw_serial_->available()) {
    this->hw_serial_->readBytes(&data, 1);
    byte = data;
  }
#endif
  return byte;
}

void ImprovSerialThreadComponent::write_data_(const uint8_t *data, const size_t size) {
  // First, set length field
  this->tx_header_[TX_LENGTH_IDX] = this->tx_header_[TX_TYPE_IDX] == TYPE_RPC_RESPONSE ? size : 1;

  const bool there_is_data = data != nullptr && size > 0;
  // If there_is_data, checksum must not include our optional data byte
  const uint8_t header_checksum_len = there_is_data ? TX_BUFFER_SIZE - 3 : TX_BUFFER_SIZE - 2;
  // Only transmit the full buffer length if there is no data (only state/error byte is provided in this case)
  const uint8_t header_tx_len = there_is_data ? TX_BUFFER_SIZE - 3 : TX_BUFFER_SIZE;
  // Calculate checksum for message
  uint8_t checksum = 0;
  for (uint8_t i = 0; i < header_checksum_len; i++) {
    checksum += this->tx_header_[i];
  }
  if (there_is_data) {
    // Include data in checksum
    for (size_t i = 0; i < size; i++) {
      checksum += data[i];
    }
  }
  this->tx_header_[TX_CHECKSUM_IDX] = checksum;

#ifdef USE_ESP32
  switch (logger::global_logger->get_uart()) {
    case logger::UART_SELECTION_UART0:
    case logger::UART_SELECTION_UART1:
#if !defined(USE_ESP32_VARIANT_ESP32C3) && !defined(USE_ESP32_VARIANT_ESP32C6) && \
    !defined(USE_ESP32_VARIANT_ESP32C61) && !defined(USE_ESP32_VARIANT_ESP32S2) && !defined(USE_ESP32_VARIANT_ESP32S3)
    case logger::UART_SELECTION_UART2:
#endif
      uart_write_bytes(this->uart_num_, this->tx_header_, header_tx_len);
      if (there_is_data) {
        uart_write_bytes(this->uart_num_, data, size);
        uart_write_bytes(this->uart_num_, &this->tx_header_[TX_CHECKSUM_IDX], 2);  // Footer: checksum and newline
      }
      break;
#if defined(USE_LOGGER_USB_CDC) && defined(CONFIG_ESP_CONSOLE_USB_CDC)
    case logger::UART_SELECTION_USB_CDC:
      esp_usb_console_write_buf((const char *) this->tx_header_, header_tx_len);
      if (there_is_data) {
        esp_usb_console_write_buf((const char *) data, size);
        esp_usb_console_write_buf((const char *) &this->tx_header_[TX_CHECKSUM_IDX],
                                  2);  // Footer: checksum and newline
      }
      break;
#endif
#ifdef USE_LOGGER_USB_SERIAL_JTAG
    case logger::UART_SELECTION_USB_SERIAL_JTAG:
      usb_serial_jtag_write_bytes((const char *) this->tx_header_, header_tx_len, 20 / portTICK_PERIOD_MS);
      if (there_is_data) {
        usb_serial_jtag_write_bytes((const char *) data, size, 20 / portTICK_PERIOD_MS);
        usb_serial_jtag_write_bytes((const char *) &this->tx_header_[TX_CHECKSUM_IDX], 2,
                                    20 / portTICK_PERIOD_MS);  // Footer: checksum and newline
      }
      break;
#endif
    default:
      break;
  }
#elif defined(USE_ARDUINO)
  this->hw_serial_->write(this->tx_header_, header_tx_len);
  if (there_is_data) {
    this->hw_serial_->write(data, size);
    this->hw_serial_->write(&this->tx_header_[TX_CHECKSUM_IDX], 2);  // Footer: checksum and newline
  }
#endif
}

std::vector<uint8_t> ImprovSerialThreadComponent::build_rpc_settings_response_(improv::Command command) {
  std::vector<std::string> urls;
#ifdef USE_IMPROV_SERIAL_THREAD_NEXT_URL
  if (!this->next_url_.empty()) {
    urls.push_back(this->get_formatted_next_url_());
  }
#endif
  
  // Add IPv6 address if available
  if (openthread::global_openthread_component != nullptr) {
    auto omr_addr = openthread::global_openthread_component->get_omr_address();
    if (omr_addr.has_value()) {
      char addr_str[40];
      snprintf(addr_str, sizeof(addr_str), "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
               omr_addr->mFields.m8[0], omr_addr->mFields.m8[1], omr_addr->mFields.m8[2], omr_addr->mFields.m8[3],
               omr_addr->mFields.m8[4], omr_addr->mFields.m8[5], omr_addr->mFields.m8[6], omr_addr->mFields.m8[7],
               omr_addr->mFields.m8[8], omr_addr->mFields.m8[9], omr_addr->mFields.m8[10], omr_addr->mFields.m8[11],
               omr_addr->mFields.m8[12], omr_addr->mFields.m8[13], omr_addr->mFields.m8[14], omr_addr->mFields.m8[15]);
#ifdef USE_WEBSERVER
      std::string webserver_url = "http://[" + std::string(addr_str) + "]:" + to_string(USE_WEBSERVER_PORT);
      urls.push_back(webserver_url);
#endif
    }
  }
  
  std::vector<uint8_t> data = improv::build_rpc_response(command, urls, false);
  return data;
}

std::vector<uint8_t> ImprovSerialThreadComponent::build_version_info_() {
#ifdef ESPHOME_PROJECT_NAME
  std::vector<std::string> infos = {ESPHOME_PROJECT_NAME, ESPHOME_PROJECT_VERSION, ESPHOME_VARIANT, App.get_name()};
#else
  std::vector<std::string> infos = {"ESPHome", ESPHOME_VERSION, ESPHOME_VARIANT, App.get_name()};
#endif
  std::vector<uint8_t> data = improv::build_rpc_response(improv::GET_DEVICE_INFO, infos, false);
  return data;
};

bool ImprovSerialThreadComponent::parse_improv_serial_byte_(uint8_t byte) {
  size_t at = this->rx_buffer_.size();
  this->rx_buffer_.push_back(byte);
  ESP_LOGV(TAG, "Byte: 0x%02X", byte);
  const uint8_t *raw = &this->rx_buffer_[0];

  return improv::parse_improv_serial_byte(
      at, byte, raw, [this](improv::ImprovCommand command) -> bool { return this->parse_improv_payload_(command); },
      [this](improv::Error error) -> void {
        ESP_LOGW(TAG, "Error decoding payload");
        this->set_error_(error);
      });
}

bool ImprovSerialThreadComponent::parse_improv_payload_(improv::ImprovCommand &command) {
  switch (command.command) {
    case THREAD_SETTINGS: {
      // Parse Thread dataset from command
      if (command.ssid.empty()) {
        ESP_LOGW(TAG, "Empty Thread dataset received");
        this->set_error_(improv::ERROR_INVALID_RPC);
        return false;
      }

      // Convert hex string to bytes
      std::vector<uint8_t> dataset_bytes;
      for (size_t i = 0; i < command.ssid.length(); i += 2) {
        std::string byte_string = command.ssid.substr(i, 2);
        uint8_t byte = (uint8_t) strtol(byte_string.c_str(), nullptr, 16);
        dataset_bytes.push_back(byte);
      }

      this->dataset_tlv_ = dataset_bytes;

      if (openthread::global_openthread_component == nullptr) {
        ESP_LOGE(TAG, "OpenThread component not available");
        this->set_error_(improv::ERROR_UNKNOWN_RPC);
        return false;
      }

      if (!openthread::global_openthread_component->set_dataset_tlv(dataset_bytes)) {
        ESP_LOGE(TAG, "Failed to set Thread dataset");
        this->set_error_(improv::ERROR_INVALID_RPC);
        return false;
      }

      if (!openthread::global_openthread_component->start_joining()) {
        ESP_LOGE(TAG, "Failed to start Thread joining");
        this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
        return false;
      }

      this->set_state_(improv::STATE_PROVISIONING);
      ESP_LOGD(TAG, "Received Thread dataset, joining network");

      auto f = std::bind(&ImprovSerialThreadComponent::on_thread_connect_timeout_, this);
      this->set_timeout("thread-connect-timeout", 60000, f);
      return true;
    }
    case improv::GET_CURRENT_STATE:
      this->set_state_(this->state_);
      if (this->state_ == improv::STATE_PROVISIONED) {
        std::vector<uint8_t> url = this->build_rpc_settings_response_(improv::GET_CURRENT_STATE);
        this->send_response_(url);
      }
      return true;
    case improv::GET_DEVICE_INFO: {
      std::vector<uint8_t> info = this->build_version_info_();
      this->send_response_(info);
      return true;
    }
    default: {
      ESP_LOGW(TAG, "Unknown payload");
      this->set_error_(improv::ERROR_UNKNOWN_RPC);
      return false;
    }
  }
}

void ImprovSerialThreadComponent::set_state_(improv::State state) {
  this->state_ = state;
  this->tx_header_[TX_TYPE_IDX] = TYPE_CURRENT_STATE;
  this->tx_header_[TX_DATA_IDX] = state;
  this->write_data_();
}

void ImprovSerialThreadComponent::set_error_(improv::Error error) {
  this->tx_header_[TX_TYPE_IDX] = TYPE_ERROR_STATE;
  this->tx_header_[TX_DATA_IDX] = error;
  this->write_data_();
}

void ImprovSerialThreadComponent::send_response_(std::vector<uint8_t> &response) {
  this->tx_header_[TX_TYPE_IDX] = TYPE_RPC_RESPONSE;
  this->write_data_(response.data(), response.size());
}

void ImprovSerialThreadComponent::on_thread_connect_timeout_() {
  this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
  this->set_state_(improv::STATE_AUTHORIZED);
  ESP_LOGW(TAG, "Timed out while joining Thread network");
  if (openthread::global_openthread_component != nullptr) {
    openthread::global_openthread_component->clear_dataset();
  }
}

ImprovSerialThreadComponent *global_improv_serial_thread_component =  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    nullptr;                                                           // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace improv_serial_thread
}  // namespace esphome
#endif
