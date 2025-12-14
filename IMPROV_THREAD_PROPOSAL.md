# Improv-Thread Protocol Proposal

## Executive Summary

This document proposes the creation of an "improv-thread" protocol for ESPHome, similar to the existing improv-wifi protocol but designed for Thread network provisioning. This would enable devices with Thread capabilities to be provisioned in a standardized way using BLE or Serial communication.

## Current State Analysis

### Improv Protocol Overview

The Improv protocol in ESPHome is a standardized provisioning protocol that allows devices to be configured without hardcoding credentials. It currently exists in two variants:
- **esp32_improv**: BLE-based provisioning over Bluetooth Low Energy
- **improv_serial**: Serial-based provisioning over UART/USB

### Architecture Components

#### 1. **improv_base** (Base Component)
**Location**: `esphome/components/improv_base/`

**Purpose**: Provides shared functionality for both BLE and Serial implementations

**Key Features**:
- URL formatting with placeholders (`{{device_name}}`, `{{ip_address}}`)
- Common base class `ImprovBase` for both implementations
- Next URL support for redirecting after provisioning

**Files**:
- `improv_base.h`: Base class definition
- `improv_base.cpp`: URL formatting logic
- `__init__.py`: Python configuration schema

#### 2. **esp32_improv** (BLE Provisioning)
**Location**: `esphome/components/esp32_improv/`

**Purpose**: Provides WiFi provisioning via Bluetooth Low Energy

**Key Components**:
- Uses standard Improv BLE GATT service (UUID-based)
- Characteristic-based communication:
  - Status characteristic (read/notify)
  - Error characteristic (read/notify)
  - RPC command characteristic (write)
  - RPC response characteristic (read/notify)
  - Capabilities characteristic (read)

**Protocol Features**:
- State machine: STOPPED → AWAITING_AUTHORIZATION → AUTHORIZED → PROVISIONING → PROVISIONED
- Authorization via binary sensor (optional)
- Status indicator via binary output
- Service data advertising with protocol ID
- Periodic device name advertising (every 60s for 1s)
- WiFi timeout configuration
- Automation triggers for state changes

**Communication Flow**:
1. Device advertises Improv BLE service
2. Client connects and reads status/capabilities
3. Optional authorization check
4. Client sends WiFi credentials via RPC
5. Device attempts connection
6. On success, returns URLs to client
7. Device transitions to PROVISIONED state

**Files**:
- `esp32_improv_component.h`: Component class definition
- `esp32_improv_component.cpp`: BLE implementation (478 lines)
- `automation.h`: Automation triggers
- `__init__.py`: Python configuration

#### 3. **improv_serial** (Serial Provisioning)
**Location**: `esphome/components/improv_serial/`

**Purpose**: Provides WiFi provisioning via Serial/UART

**Protocol Features**:
- Frame-based protocol with checksum
- Commands: WIFI_SETTINGS, GET_CURRENT_STATE, GET_DEVICE_INFO, GET_WIFI_NETWORKS
- Works over UART, USB CDC, USB Serial JTAG
- Similar state machine to BLE version but simplified (no authorization state)

**Communication Flow**:
1. Client sends commands over serial
2. Device responds with formatted frames
3. WiFi network scanning support
4. Provisioning flow similar to BLE

**Files**:
- `improv_serial_component.h`: Component class definition
- `improv_serial_component.cpp`: Serial implementation (315 lines)
- `__init__.py`: Python configuration

### WiFi Component Integration

**Location**: `esphome/components/wifi/`

**Key Integration Points**:
- `wifi::global_wifi_component`: Global WiFi component instance
- `wifi::WiFiAP`: Access point credentials structure
- Methods used by Improv:
  - `has_sta()`: Check if credentials exist
  - `set_sta()`: Set WiFi credentials
  - `start_connecting()`: Begin connection attempt
  - `save_wifi_sta()`: Persist credentials
  - `is_connected()`: Check connection status
  - `clear_sta()`: Clear credentials on failure
  - `wifi_sta_ip_addresses()`: Get assigned IP addresses
  - `get_scan_result()`: Get scanned networks (serial only)

### OpenThread Component

**Location**: `esphome/components/openthread/`

**Current Functionality**:
- Thread network initialization and management
- SRP (Service Registration Protocol) for mDNS-like service discovery
- Network dataset configuration via YAML
- Device types: FTD (Full Thread Device), MTD (Minimal Thread Device)
- Poll period configuration for sleepy devices
- Factory reset support

**Key Differences from WiFi**:
- Thread uses dataset-based configuration (not SSID/password)
- Dataset contains: PAN ID, Channel, Network Key, Extended PAN ID, Network Name, PSKC, Mesh Local Prefix
- Thread supports TLV (Type-Length-Value) format for full dataset
- No "scanning" equivalent - Thread uses mesh discovery
- IPv6 only (no IPv4)
- OMR (Off-Mesh Routable) addresses instead of DHCP

**Files**:
- `openthread.h`: Component class definition
- `openthread.cpp`: Core Thread implementation
- `openthread_esp.cpp`: ESP32-specific code
- `const.py`: Configuration constants
- `__init__.py`: Python configuration

## Target State: Improv-Thread Protocol

### Proposed Architecture

The improv-thread protocol should mirror the existing improv-wifi structure but adapted for Thread network provisioning:

```
improv_base/               (existing, may need minor updates)
├── improv_base.h
├── improv_base.cpp
└── __init__.py

esp32_improv_thread/       (NEW - BLE-based Thread provisioning)
├── esp32_improv_thread_component.h
├── esp32_improv_thread_component.cpp
├── automation.h
└── __init__.py

improv_serial_thread/      (NEW - Serial-based Thread provisioning)
├── improv_serial_thread_component.h
├── improv_serial_thread_component.cpp
└── __init__.py
```

### Protocol Specification

#### Commands

Based on the Improv protocol structure, the Thread variant would need:

**RPC Commands** (similar to WiFi):
- `THREAD_SETTINGS` (0x01): Provision Thread network
  - Input: Thread dataset (TLV format or individual parameters)
  - Output: URLs on success
- `GET_CURRENT_STATE` (0x02): Query current state
  - Output: Current state + URLs if provisioned
- `GET_DEVICE_INFO` (0x03): Get device information
  - Output: Device name, version, capabilities
- `GET_THREAD_NETWORKS` (0x04): Scan for Thread networks (if supported)
  - Output: List of discovered Thread networks with metadata

**States** (reuse existing):
- `STATE_STOPPED` (0x00): Service not running
- `STATE_AWAITING_AUTHORIZATION` (0x01): Waiting for user authorization
- `STATE_AUTHORIZED` (0x02): Ready to receive credentials
- `STATE_PROVISIONING` (0x03): Attempting to join Thread network
- `STATE_PROVISIONED` (0x04): Successfully joined

**Error Codes** (adapt existing):
- `ERROR_NONE` (0x00)
- `ERROR_INVALID_RPC` (0x01)
- `ERROR_UNKNOWN_RPC` (0x02)
- `ERROR_UNABLE_TO_CONNECT` (0x03)
- `ERROR_NOT_AUTHORIZED` (0x04)
- `ERROR_INVALID_DATASET` (0x05): New - invalid Thread dataset

#### Data Format

**Thread Dataset Representation**:
The protocol should support two modes:

1. **TLV Format** (Recommended - most flexible):
   - Pass complete Thread operational dataset as hex string
   - Format: Type-Length-Value encoded binary data
   - Matches OpenThread's standard dataset format
   - Example: Thread Commissioner apps already generate this

2. **Individual Parameters** (Alternative - more user-friendly):
   - Network Name (string, max 16 chars)
   - PAN ID (uint16)
   - Extended PAN ID (uint64)
   - Network Key (16 bytes, hex)
   - Channel (uint8, 11-26)
   - PSKC (16 bytes, hex)
   - Mesh Local Prefix (IPv6 prefix)

**BLE Characteristic UUIDs** (New namespace for Thread):
```
Service UUID:       00467768-6228-2272-4663-277478268000  (Thread variant)
Status UUID:        00467768-6228-2272-4663-277478268001
Error UUID:         00467768-6228-2272-4663-277478268002
RPC Command UUID:   00467768-6228-2272-4663-277478268003
RPC Response UUID:  00467768-6228-2272-4663-277478268004
Capabilities UUID:  00467768-6228-2272-4663-277478268005
```

**Note on UUID Generation**: These UUIDs are derived from the improv-wifi UUIDs but modified to create a unique namespace for Thread. The base UUID follows Bluetooth SIG's 128-bit UUID format. These should be coordinated with the improv protocol maintainers to avoid conflicts and ensure proper registration. The final UUIDs may be assigned by the Improv protocol specification once Thread support is standardized.

**Service Data Format** (8 bytes):
```
Byte 0-1: Protocol ID (0x54 0x48 = "TH")
Byte 2:   State
Byte 3:   Capabilities (bit 0: identify support)
Byte 4-7: Reserved
```

#### URLs After Provisioning

After successful Thread provisioning, the device should return:
1. Custom next_url (if configured)
2. IPv6 address URL (if webserver enabled): `http://[fd00::1234]:80`
3. mDNS URL: `http://devicename.local:80`
4. Home Assistant integration URL (if configured)

### Component Structure

#### esp32_improv_thread Component

**Class: ESP32ImprovThreadComponent**

Inherits from: `Component`, `improv_base::ImprovBase`

**Key Methods**:
- `setup()`: Initialize BLE service and characteristics
- `loop()`: State machine and connection monitoring
- `dump_config()`: Log configuration
- `start()`: Begin provisioning service
- `stop()`: Stop provisioning service
- `set_state_()`: Update state and notify clients
- `set_error_()`: Set error state
- `process_incoming_data_()`: Parse RPC commands
- `check_thread_connection_()`: Monitor Thread join progress
- `on_thread_connect_timeout_()`: Handle join timeout

**Dependencies**:
- `esp32_ble_server`: BLE GATT server
- `openthread`: Thread network component
- `improv_base`: Base functionality
- Optional: `binary_sensor` (authorization), `output` (status indicator)

**Configuration Options**:
```yaml
esp32_improv_thread:
  authorizer: some_binary_sensor  # Optional authorization
  status_indicator: some_output    # Optional LED indicator
  identify_duration: 10s           # How long identify lasts
  authorized_duration: 60s         # Authorization timeout
  thread_timeout: 60s              # Thread join timeout
  next_url: "http://example.com"   # Redirect after provisioning
  on_provisioned:                  # Automation trigger
    - logger.log: "Thread provisioned!"
  on_provisioning:
    - logger.log: "Joining Thread network..."
  on_start:
    - logger.log: "Improv Thread started"
  on_state:
    - logger.log: "State changed"
  on_stop:
    - logger.log: "Improv Thread stopped"
```

#### improv_serial_thread Component

**Class: ImprovSerialThreadComponent**

Inherits from: `Component`, `improv_base::ImprovBase`

**Key Methods**:
- `setup()`: Initialize serial communication
- `loop()`: Read serial data and check Thread status
- `parse_improv_serial_byte_()`: Parse incoming serial frames
- `parse_improv_payload_()`: Handle RPC commands
- `set_state_()`: Update state
- `set_error_()`: Set error state
- `send_response_()`: Send response frame
- `build_rpc_settings_response_()`: Build provisioning response

**Dependencies**:
- `logger`: Serial port access
- `openthread`: Thread network component
- `improv_base`: Base functionality

**Configuration Options**:
```yaml
improv_serial_thread:
  next_url: "http://example.com"   # Redirect after provisioning
```

### Integration with OpenThread Component

The improv-thread components need to integrate with the OpenThread component differently than improv-wifi integrates with WiFi:

**Key Differences**:
1. **Dataset vs Credentials**: Thread uses operational dataset instead of SSID/password
2. **No Scanning**: Thread networks aren't typically scanned like WiFi (mesh discovery is different)
3. **IPv6 Only**: Must handle IPv6 addresses instead of IPv4
4. **Join Process**: Thread joining is async and may take longer than WiFi
5. **State Persistence**: Thread credentials should be stored differently - Thread uses a binary operational dataset (~60-100 bytes) instead of separate SSID and password strings. The dataset should be stored as a single blob in NVS (non-volatile storage) using ESPHome's preferences API, similar to how WiFi stores credentials but with a different key namespace

**Required OpenThread API Additions**:

Add to `OpenThreadComponent` class:

```cpp
// Set Thread dataset from TLV hex string
bool set_dataset_tlv(const std::string &tlv_hex);

// Set Thread dataset from individual parameters
bool set_dataset_params(const std::string &network_name,
                       uint16_t pan_id,
                       uint64_t ext_pan_id,
                       const std::vector<uint8_t> &network_key,
                       uint8_t channel,
                       const std::vector<uint8_t> &pskc);

// Clear Thread credentials
void clear_dataset();

// Check if dataset is configured
bool has_dataset();

// Save current dataset to non-volatile storage
bool save_dataset();

// Start joining Thread network
bool start_joining();

// Get current joining state
enum JoinState { NOT_JOINED, JOINING, JOINED };
JoinState get_join_state();

// Check if actively joined to a Thread network
bool is_joined();  // Different from is_connected() - more immediate

// Get callback for join state changes
void set_on_join_callback(std::function<void(JoinState)> callback);
```

**Integration Flow**:

1. Improv component receives dataset
2. Calls `openthread_component->set_dataset_tlv()` or `set_dataset_params()`
3. Calls `openthread_component->start_joining()`
4. Monitors `get_join_state()` or uses callback
5. On success, calls `save_dataset()`
6. Returns URLs to client

### Capability Detection

The improv-thread components should only be available on platforms that support Thread:

**Supported Platforms**:
- ESP32-C6 (tested and available)
- ESP32-H2 (tested and available)
- ESP32-C5 (preliminary support in ESP-IDF, limited hardware availability as of late 2024)

**Platform Detection** (in Python config):
```python
from esphome.components.esp32 import (
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    only_on_variant,
)

CONFIG_SCHEMA = cv.All(
    cv.Schema({...}),
    only_on_variant(supported=[VARIANT_ESP32C5, VARIANT_ESP32C6, VARIANT_ESP32H2]),
    cv.only_with_esp_idf,
)
```

## Implementation Instructions

### Phase 1: Extend OpenThread Component

**Files to Modify**:
- `esphome/components/openthread/openthread.h`
- `esphome/components/openthread/openthread.cpp`

**Tasks**:
1. Add dataset management methods (set_dataset_tlv, set_dataset_params, clear_dataset, has_dataset, save_dataset)
2. Add join state tracking and callbacks
3. Add methods to start joining and check join status
4. Implement non-volatile storage for dataset (use ESPHome preferences API)
5. Add error handling for invalid datasets

**Estimated Lines**: ~300 lines of new code

### Phase 2: Create improv_base Updates

**Files to Modify**:
- `esphome/components/improv_base/improv_base.h` (minor updates if needed)
- `esphome/components/improv_base/__init__.py` (add Thread-specific schema)

**Tasks**:
1. Review if any Thread-specific base functionality is needed
2. Consider adding Thread-specific URL formatting (IPv6)
3. Update Python schema if needed

**Estimated Lines**: ~50 lines

### Phase 3: Create esp32_improv_thread Component

**New Files**:
- `esphome/components/esp32_improv_thread/esp32_improv_thread_component.h`
- `esphome/components/esp32_improv_thread/esp32_improv_thread_component.cpp`
- `esphome/components/esp32_improv_thread/automation.h`
- `esphome/components/esp32_improv_thread/__init__.py`

**Tasks**:
1. Copy esp32_improv component as template
2. Replace WiFi-specific code with Thread equivalents
3. Update UUIDs for Thread-specific service
4. Implement dataset parsing and validation
5. Update service data advertising for Thread protocol ID
6. Implement Thread join monitoring
7. Update response building for IPv6 URLs
8. Create automation triggers
9. Write Python configuration schema

**Estimated Lines**: ~600 lines total

### Phase 4: Create improv_serial_thread Component

**New Files**:
- `esphome/components/improv_serial_thread/improv_serial_thread_component.h`
- `esphome/components/improv_serial_thread/improv_serial_thread_component.cpp`
- `esphome/components/improv_serial_thread/__init__.py`

**Tasks**:
1. Copy improv_serial component as template
2. Replace WiFi-specific code with Thread equivalents
3. Update command handling for Thread dataset
4. Remove or adapt GET_WIFI_NETWORKS command
5. Update response building for IPv6 URLs
6. Write Python configuration schema

**Estimated Lines**: ~400 lines total

### Phase 5: Update Improv Protocol Library (External)

**Note**: The Improv protocol library (https://github.com/improv-wifi/sdk-cpp) would need to be extended for Thread support.

**New File** (external library):
- `improv_thread.h` (similar to existing improv.h)

**Tasks**:
1. Define Thread-specific constants (UUIDs, protocol IDs)
2. Define THREAD_SETTINGS command structure
3. Add dataset parsing utilities
4. Submit PR to improv-wifi/sdk-cpp project

**Estimated Lines**: ~200 lines

### Phase 6: Testing and Documentation

**Testing Tasks**:
1. Unit tests for dataset parsing
2. Integration tests for BLE provisioning
3. Integration tests for Serial provisioning
4. Test with different Thread border routers
5. Test authorization flow
6. Test error conditions (invalid dataset, join timeout, etc.)
7. Test IPv6 URL generation

**Documentation Tasks**:
1. Component documentation for esp32_improv_thread
2. Component documentation for improv_serial_thread
3. Configuration examples
4. Client application guidance
5. Thread Border Router setup guide

## Technical Considerations

### Security

1. **BLE Security**: Same as improv-wifi - relies on BLE pairing
2. **Dataset Confidentiality**: Network key transmitted over BLE should be encrypted
3. **Authorization**: Support binary sensor authorization like WiFi variant
4. **Factory Reset**: Ensure Thread credentials cleared on factory reset

### Performance

1. **Join Time**: Thread joining can take 10-60 seconds (longer than WiFi)
   - Adjust default timeout to 60s (compared to improv-wifi's timeout which is configurable with a default of 90s in the WiFi component, but improv uses a 30s RPC-level timeout)
2. **Memory**: Thread datasets are ~60-100 bytes
3. **BLE MTU**: Ensure dataset fits in BLE packet size (max 512 bytes)

### Compatibility

1. **Platform Support**: ESP32-C6, ESP32-H2 (fully supported); ESP32-C5 (preliminary support, may need updates as hardware becomes available)
2. **IDF Version**: Requires ESP-IDF 5.1+ for Thread support
3. **Conflicts**: Cannot use with WiFi simultaneously (radio conflict)

### Edge Cases

1. **Already Provisioned**: If Thread dataset exists, should improv-thread start?
   - Recommendation: Same behavior as WiFi - don't start if already provisioned
2. **Multiple Border Routers**: Thread supports multiple paths
3. **Network Migration**: Changing Thread networks requires clearing old dataset
4. **Sleepy Devices**: MTD with poll_period should work but may have delayed joins

## Migration Path

For users with existing WiFi setups:

1. **Dual-Protocol Devices**: Not possible on same hardware (radio conflict)
2. **Configuration Migration**: 
   - Remove `wifi:` component
   - Add `openthread:` component
   - Replace `esp32_improv:` with `esp32_improv_thread:`
3. **Client Apps**: Need new app or app update for Thread provisioning

## Alternative Approaches Considered

### 1. Unified Improv Component
**Idea**: Single component that handles both WiFi and Thread

**Pros**: Less code duplication, simpler user config

**Cons**: 
- Increased complexity
- Different data formats and flows
- Cannot use both simultaneously anyway

**Decision**: Rejected - separate components maintain clarity

### 2. Extend Existing Improv WiFi
**Idea**: Add Thread support to existing esp32_improv

**Pros**: Reuses existing codebase

**Cons**:
- Conceptually different protocols
- Would require lots of conditional compilation
- Harder to maintain
- UUIDs should be different for different purposes

**Decision**: Rejected - new components clearer

### 3. Configuration-Time Dataset Only
**Idea**: No runtime provisioning, only YAML config

**Pros**: Simpler implementation

**Cons**:
- Defeats purpose of improv protocol
- No user-friendly provisioning
- Requires recompile to change networks

**Decision**: Rejected - provisioning is the goal

## Client Application Requirements

For improv-thread to be useful, client applications need to be created or updated:

### BLE Client App Requirements

1. **Thread Dataset Generation**: App must generate valid Thread operational dataset
2. **BLE Discovery**: Scan for improv-thread BLE service UUID
3. **Characteristic Access**: Read/write characteristics per protocol
4. **Dataset Input UI**: 
   - Option 1: Thread Border Router QR code scan → extract dataset
   - Option 2: Manual parameter entry
   - Option 3: Thread Commissioner protocol integration
5. **Status Feedback**: Display provisioning progress
6. **Error Handling**: Show meaningful errors to user

### Suggested Client Platforms

1. **iOS App**: Swift app using CoreBluetooth
2. **Android App**: Kotlin app using Android BLE APIs
3. **Web App**: Web Bluetooth API (Chrome/Edge)
4. **Python CLI**: For development/testing

## Success Criteria

1. ✅ ESP32-C6/H2 device can be provisioned to Thread network via BLE
2. ✅ ESP32-C6/H2 device can be provisioned to Thread network via Serial
3. ✅ Provisioning works with standard Thread Border Routers
4. ✅ Device successfully joins Thread network after provisioning
5. ✅ URLs returned include correct IPv6 addresses
6. ✅ Authorization flow works correctly
7. ✅ Error conditions handled gracefully
8. ✅ Credentials persisted across reboots
9. ✅ Factory reset clears Thread credentials
10. ✅ Documentation complete and clear

## Timeline Estimate

**Assuming 1 developer working part-time:**

- Phase 1 (OpenThread): 1-2 weeks
- Phase 2 (improv_base): 2-3 days
- Phase 3 (esp32_improv_thread): 2-3 weeks
- Phase 4 (improv_serial_thread): 1-2 weeks
- Phase 5 (External library): 1 week (coordinate with improv maintainers)
- Phase 6 (Testing/Docs): 2-3 weeks

**Total**: 8-12 weeks

## Open Questions

1. **Protocol Standardization**: Should this be standardized with the improv-wifi project?
   - Recommendation: Yes, coordinate with improv-wifi maintainers
2. **Thread Network Scanning**: Is there a Thread equivalent to WiFi scanning?
   - Answer: Thread discovery is different - devices discover via mesh, not active scanning. Thread devices can detect existing networks during the joining process, but there's no equivalent to WiFi's active scan before connection.
   - Recommendation: The GET_THREAD_NETWORKS command (listed in the protocol spec) should be implemented as optional/informational only, returning networks discovered during recent join attempts. However, it should NOT be relied upon for network selection like WiFi scanning. Initial implementation can omit this command entirely and add it in a future version if Thread Border Router discovery mechanisms become standardized
3. **Multiple Datasets**: Should device support multiple Thread network profiles?
   - Recommendation: Start with single dataset, add multi-network later if needed
4. **Commissioner Integration**: Should this integrate with Thread Commissioner protocol?
   - Recommendation: Phase 2 feature - start with dataset provisioning
5. **Backwards Compatibility**: How to handle devices that support both WiFi and Thread (different hardware)?
   - Recommendation: User chooses one at compile time, cannot mix

## References

1. ESPHome WiFi Component: `/esphome/components/wifi/`
2. ESPHome OpenThread Component: `/esphome/components/openthread/`
3. ESPHome ESP32 Improv: `/esphome/components/esp32_improv/`
4. ESPHome Improv Serial: `/esphome/components/improv_serial/`
5. Improv WiFi Protocol: https://www.improv-wifi.com/
6. Thread Specification: https://www.threadgroup.org/
7. OpenThread: https://openthread.io/
8. Thread Border Router: https://openthread.io/guides/border-router

## Conclusion

The improv-thread protocol would provide a standardized, user-friendly way to provision Thread network credentials to ESPHome devices. By following the proven pattern of improv-wifi, we can create a consistent experience for users while adapting to Thread's unique characteristics.

The implementation requires:
- Extension of OpenThread component for dynamic provisioning
- Two new components (BLE and Serial variants)
- Client application development
- Coordination with improv protocol maintainers
- Comprehensive testing

This proposal provides a clear roadmap for implementation while acknowledging technical challenges and providing solutions.
