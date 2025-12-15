#include "improv_base.h"

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/defines.h"

namespace esphome {
namespace improv_base {

#if defined(USE_ESP32_IMPROV_NEXT_URL) || defined(USE_IMPROV_SERIAL_NEXT_URL)
static constexpr const char DEVICE_NAME_PLACEHOLDER[] = "{{device_name}}";
static constexpr size_t DEVICE_NAME_PLACEHOLDER_LEN = sizeof(DEVICE_NAME_PLACEHOLDER) - 1;
static constexpr const char IP_ADDRESS_PLACEHOLDER[] = "{{ip_address}}";
static constexpr size_t IP_ADDRESS_PLACEHOLDER_LEN = sizeof(IP_ADDRESS_PLACEHOLDER) - 1;

static void replace_all_in_place(std::string &str, const char *placeholder, size_t placeholder_len,
                                 const std::string &replacement) {
  size_t pos = 0;
  const size_t replacement_len = replacement.length();
  while ((pos = str.find(placeholder, pos)) != std::string::npos) {
    str.replace(pos, placeholder_len, replacement);
    pos += replacement_len;
  }
}

std::string ImprovBase::get_formatted_next_url_() {
  if (this->next_url_.empty()) {
    return "";
  }

  std::string formatted_url = this->next_url_;

  // Replace all occurrences of {{device_name}}
  replace_all_in_place(formatted_url, DEVICE_NAME_PLACEHOLDER, DEVICE_NAME_PLACEHOLDER_LEN, App.get_name());

  // Replace all occurrences of {{ip_address}}
  for (auto &ip : network::get_ip_addresses()) {
    if (ip.is_ip4()) {
      replace_all_in_place(formatted_url, IP_ADDRESS_PLACEHOLDER, IP_ADDRESS_PLACEHOLDER_LEN, ip.str());
      break;
    }
  }

  // Note: {{esphome_version}} is replaced at code generation time in Python

  return formatted_url;
}
#endif

#ifdef USE_OPENTHREAD
#include <cctype>

std::string ImprovBase::format_ipv6_address(const otIp6Address &addr) {
  // Buffer size of 64 provides ample space for full IPv6 address
  char addr_str[64];
  snprintf(addr_str, sizeof(addr_str), "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
           addr.mFields.m8[0], addr.mFields.m8[1], addr.mFields.m8[2], addr.mFields.m8[3], addr.mFields.m8[4],
           addr.mFields.m8[5], addr.mFields.m8[6], addr.mFields.m8[7], addr.mFields.m8[8], addr.mFields.m8[9],
           addr.mFields.m8[10], addr.mFields.m8[11], addr.mFields.m8[12], addr.mFields.m8[13], addr.mFields.m8[14],
           addr.mFields.m8[15]);
  return std::string(addr_str);
}

bool ImprovBase::parse_hex_string_to_bytes(const std::string &hex_string, std::vector<uint8_t> &output,
                                           std::string &error_msg) {
  output.clear();
  
  // Validate hex string format
  if (hex_string.length() % 2 != 0) {
    error_msg = "Invalid hex string length (must be even)";
    return false;
  }

  // Convert hex string to bytes
  for (size_t i = 0; i + 1 < hex_string.length(); i += 2) {
    std::string byte_string = hex_string.substr(i, 2);
    // Validate we got 2 characters and both are hex
    if (byte_string.length() != 2 || !std::isxdigit(byte_string[0]) || !std::isxdigit(byte_string[1])) {
      error_msg = "Invalid hex character in string";
      return false;
    }
    uint8_t byte = (uint8_t) strtol(byte_string.c_str(), nullptr, 16);
    output.push_back(byte);
  }
  
  return true;
}
#endif

}  // namespace improv_base
}  // namespace esphome
