# Improv-Thread Implementation Summary

This document summarizes the implementation of the improv-thread protocol in ESPHome, enabling Thread network provisioning via BLE or Serial interfaces.

## Overview

The improv-thread protocol provides a standardized way to provision Thread network credentials to ESP32 devices with Thread support (C5, C6, H2), similar to the existing improv-wifi protocol but adapted for Thread mesh networks.

## Components Implemented

### 1. OpenThread Runtime Provisioning APIs

**Location**: `esphome/components/openthread/`

**Additions to `OpenThreadComponent`**:

```cpp
// Dataset management
bool set_dataset_tlv(const std::vector<uint8_t> &tlv_data);
bool set_dataset_params(const std::string &network_name, uint16_t pan_id, 
                       uint64_t ext_pan_id, const std::vector<uint8_t> &network_key,
                       uint8_t channel, const std::vector<uint8_t> &pskc);
void clear_dataset();
bool has_dataset();
bool save_dataset();

// Join management
bool start_joining();
JoinState get_join_state();  // NOT_JOINED, JOINING, JOINED, JOIN_FAILED
bool is_joined();
void set_on_join_callback(std::function<void(JoinState)> callback);
```

**Key Features**:
- Supports both TLV format and individual parameter setting
- Handles Thread operational dataset configuration
- Manages IPv6 interface and Thread protocol startup
- Tracks join state with callback support
- Network byte order (big-endian) handling for ext_pan_id

### 2. ESP32 Improv Thread (BLE-based)

**Location**: `esphome/components/esp32_improv_thread/`

**Files**:
- `esp32_improv_thread_component.h` - Component class definition
- `esp32_improv_thread_component.cpp` - BLE implementation
- `automation.h` - Automation triggers
- `__init__.py` - Python configuration

**Features**:
- BLE GATT service for Thread provisioning
- Improv protocol state machine (STOPPED → AWAITING_AUTHORIZATION → AUTHORIZED → PROVISIONING → PROVISIONED)
- Thread dataset provisioning via hex-encoded TLV format
- Authorization support via binary sensor
- Status indicator via binary output
- Configurable Thread join timeout (default 60s)
- Automation triggers: `on_provisioned`, `on_provisioning`, `on_start`, `on_state`, `on_stop`
- Service data advertising with Thread protocol ID (0x54 0x48 = "TH")
- Periodic device name advertising

**Configuration Example**:
```yaml
esp32_improv_thread:
  authorizer: some_binary_sensor  # Optional
  status_indicator: some_output    # Optional
  identify_duration: 10s
  authorized_duration: 60s
  thread_timeout: 60s
  next_url: "http://example.com"
  on_provisioned:
    - logger.log: "Thread network provisioned!"
  on_provisioning:
    - logger.log: "Joining Thread network..."
```

### 3. Improv Serial Thread (Serial-based)

**Location**: `esphome/components/improv_serial_thread/`

**Files**:
- `improv_serial_thread_component.h` - Component class definition
- `improv_serial_thread_component.cpp` - Serial implementation
- `__init__.py` - Python configuration

**Features**:
- Serial/UART-based Thread provisioning
- Works over UART, USB CDC, USB Serial JTAG
- Improv serial protocol framing
- Thread dataset provisioning via hex-encoded TLV format
- Simpler state machine (no authorization states)
- Commands: THREAD_SETTINGS, GET_CURRENT_STATE, GET_DEVICE_INFO

**Configuration Example**:
```yaml
improv_serial_thread:
  next_url: "http://example.com"
```

### 4. Shared Utilities

**Location**: `esphome/components/improv_base/`

**Additions**:
- `format_ipv6_address()` - Utility function for formatting IPv6 addresses
- Thread-specific next_url support

## Protocol Specification

### Thread Dataset Format

Thread datasets are transmitted as hex-encoded TLV (Type-Length-Value) format:
- Example: `0e080000000000010000000300000f35060004001fffe00208fedcba9876543210...`
- Contains: Network Key, PAN ID, Extended PAN ID, Channel, Network Name, PSKc, etc.

### RPC Commands

- **THREAD_SETTINGS (0x01)**: Provision Thread network with dataset
  - Input: Hex-encoded TLV dataset in `ssid` field
  - Output: URLs on success (next_url, webserver URL with IPv6)
  
- **GET_CURRENT_STATE (0x02)**: Query current state
  - Output: Current state + URLs if provisioned
  
- **GET_DEVICE_INFO (0x03)**: Get device information
  - Output: Device name, version, variant

### State Machine

```
STOPPED → AWAITING_AUTHORIZATION → AUTHORIZED → PROVISIONING → PROVISIONED
```

- **STOPPED**: Service not running
- **AWAITING_AUTHORIZATION**: Waiting for user authorization (BLE only)
- **AUTHORIZED**: Ready to receive credentials
- **PROVISIONING**: Joining Thread network
- **PROVISIONED**: Successfully joined

### Service Data (BLE)

8-byte format:
```
Byte 0-1: Protocol ID (0x54 0x48 = "TH")
Byte 2:   State
Byte 3:   Capabilities (bit 0: identify support)
Byte 4-7: Reserved
```

## Platform Support

### Supported Hardware
- ESP32-C6 (Thread 1.3 certified, production-ready) ✅
- ESP32-H2 (Thread 1.3 certified, production-ready) ✅
- ESP32-C5 (Thread support in development, verify before production use) ⚠️

### Requirements
- ESP-IDF framework (required for Thread support)
- OpenThread component configured
- IPv6 enabled in network component
- For BLE variant: ESP32 BLE server component
- For Serial variant: Logger configured with non-zero baud rate

## Complete Configuration Example

```yaml
esphome:
  name: thread-device
  friendly_name: Thread Provisioning Device

esp32:
  board: esp32-c6-devkitc-1
  variant: esp32c6
  framework:
    type: esp-idf

logger:

network:
  enable_ipv6: true

mdns:

# OpenThread configuration
openthread:
  network_key: 0x00112233445566778899aabbccddeeff  # Will be overridden by improv
  channel: 15
  pan_id: 0x1234

# BLE-based provisioning
esp32_ble_server:

esp32_improv_thread:
  authorizer: none
  thread_timeout: 60s
  on_provisioned:
    - logger.log: "Thread provisioned!"

# OR Serial-based provisioning
improv_serial_thread:
  next_url: "http://example.com"
```

## Security Considerations

1. **Authorization**: BLE variant supports binary sensor authorization
2. **Dataset Security**: Thread network key transmitted over BLE should use BLE pairing
3. **Credential Storage**: Datasets persisted in NVS automatically by OpenThread
4. **Factory Reset**: Clear Thread credentials via `clear_dataset()`

## Implementation Details

### Input Validation
- Hex string validation: checks for even length and valid hex characters
- Dataset size validation: enforces OpenThread TLV size limits
- Network name: null-termination ensured, length checked

### Endianness Handling
- Extended PAN ID: converted to network byte order (big-endian)
- All multi-byte fields follow Thread specification requirements

### Code Quality
- Duplicate code extracted to shared utilities
- Error handling for all OpenThread API calls
- Comprehensive logging at debug/info/warning levels

## Testing

### Test Configurations
Located in `tests/components/*/`:
- `esp32_improv_thread/test.esp32-c6-idf.yaml` - BLE variant
- `improv_serial_thread/test.esp32-c6-idf.yaml` - Serial variant

### Manual Testing Requirements
1. Thread Border Router running on network
2. Client application that can:
   - Generate Thread operational datasets
   - Connect via BLE or Serial
   - Send dataset in hex-encoded format
3. Verify:
   - Device joins Thread network
   - IPv6 address obtained
   - URLs returned correctly
   - Automation triggers fire

## Known Limitations

1. **No Network Scanning**: Unlike WiFi, Thread doesn't support pre-join network scanning
2. **IPv6 Only**: Thread is IPv6-only, no IPv4 support
3. **Platform Specific**: Only works on ESP32-C5/C6/H2
4. **Client Apps**: No official client apps yet (custom development needed)
5. **Dataset Format**: Currently only supports TLV format, not individual parameters via RPC

## Future Enhancements

1. **Client Applications**: Develop iOS/Android/Web apps for provisioning
2. **Thread Commissioner**: Integrate with Thread Commissioner protocol
3. **Multiple Networks**: Support switching between Thread networks
4. **Enhanced Discovery**: Add Thread network discovery mechanisms if standardized
5. **Parameter-based RPC**: Support individual parameter provisioning (not just TLV)

## Migration from Proposal

This implementation follows the architecture outlined in `IMPROV_THREAD_PROPOSAL.md`:
- ✅ Phase 1: OpenThread runtime provisioning APIs
- ⏭️ Phase 2: improv_base updates (minimal changes needed)
- ✅ Phase 3: esp32_improv_thread component
- ✅ Phase 4: improv_serial_thread component
- ⏳ Phase 5: Testing and validation (requires hardware)
- ⏳ Phase 6: Documentation (ESPHome docs PR needed)

## Statistics

- **Lines of Code**: ~1,600 new lines
- **Files Added**: 9 new files
- **Components Modified**: 2 (OpenThread, improv_base)
- **Test Configurations**: 2 YAML files

## Contributing

To contribute to this implementation:
1. Test with real Thread Border Routers
2. Develop client applications
3. Report bugs or edge cases
4. Improve error handling
5. Add more comprehensive tests

## References

- Original Proposal: `IMPROV_THREAD_PROPOSAL.md`
- Thread Specification: https://www.threadgroup.org/
- OpenThread: https://openthread.io/
- Improv WiFi Protocol: https://www.improv-wifi.com/
- ESPHome OpenThread: https://esphome.io/components/openthread.html
