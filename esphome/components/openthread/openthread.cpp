#include "esphome/core/defines.h"
#ifdef USE_OPENTHREAD
#include "openthread.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
#include "esp_openthread.h"
#endif

#include <freertos/portmacro.h>

#include <openthread/cli.h>
#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/logging.h>
#include <openthread/netdata.h>
#include <openthread/tasklet.h>

#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

static const char *const TAG = "openthread";

namespace esphome {
namespace openthread {

OpenThreadComponent *global_openthread_component =  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    nullptr;                                        // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

OpenThreadComponent::OpenThreadComponent() { global_openthread_component = this; }

void OpenThreadComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Open Thread:");
#if CONFIG_OPENTHREAD_FTD
  ESP_LOGCONFIG(TAG, "  Device Type: FTD");
#elif CONFIG_OPENTHREAD_MTD
  ESP_LOGCONFIG(TAG, "  Device Type: MTD");
  // TBD: Synchronized Sleepy End Device
  if (this->poll_period > 0) {
    ESP_LOGCONFIG(TAG, "  Device is configured as Sleepy End Device (SED)");
    uint32_t duration = this->poll_period / 1000;
    ESP_LOGCONFIG(TAG, "  Poll Period: %" PRIu32 "s", duration);
  } else {
    ESP_LOGCONFIG(TAG, "  Device is configured as Minimal End Device (MED)");
  }
#endif
}

bool OpenThreadComponent::is_connected() {
  auto lock = InstanceLock::try_acquire(100);
  if (!lock) {
    ESP_LOGW(TAG, "Failed to acquire OpenThread lock in is_connected");
    return false;
  }

  otInstance *instance = lock->get_instance();
  if (instance == nullptr) {
    return false;
  }

  otDeviceRole role = otThreadGetDeviceRole(instance);

  // TODO: If we're a leader, check that there is at least 1 known peer
  return role >= OT_DEVICE_ROLE_CHILD;
}

// Gets the off-mesh routable address
std::optional<otIp6Address> OpenThreadComponent::get_omr_address() {
  InstanceLock lock = InstanceLock::acquire();
  return this->get_omr_address_(lock);
}

std::optional<otIp6Address> OpenThreadComponent::get_omr_address_(InstanceLock &lock) {
  otNetworkDataIterator iterator = OT_NETWORK_DATA_ITERATOR_INIT;
  otInstance *instance = nullptr;

  instance = lock.get_instance();

  otBorderRouterConfig config;
  if (otNetDataGetNextOnMeshPrefix(instance, &iterator, &config) != OT_ERROR_NONE) {
    return std::nullopt;
  }

  const otIp6Prefix *omr_prefix = &config.mPrefix;
  const otNetifAddress *unicast_addresses = otIp6GetUnicastAddresses(instance);
  for (const otNetifAddress *addr = unicast_addresses; addr; addr = addr->mNext) {
    const otIp6Address *local_ip = &addr->mAddress;
    if (otIp6PrefixMatch(&omr_prefix->mPrefix, local_ip)) {
      return *local_ip;
    }
  }
  return {};
}

void OpenThreadComponent::defer_factory_reset_external_callback() {
  ESP_LOGD(TAG, "Defer factory_reset_external_callback_");
  this->defer([this]() { this->factory_reset_external_callback_(); });
}

void OpenThreadSrpComponent::srp_callback(otError err, const otSrpClientHostInfo *host_info,
                                          const otSrpClientService *services,
                                          const otSrpClientService *removed_services, void *context) {
  if (err != 0) {
    ESP_LOGW(TAG, "SRP client reported an error: %s", otThreadErrorToString(err));
    for (const otSrpClientHostInfo *host = host_info; host; host = nullptr) {
      ESP_LOGW(TAG, "  Host: %s", host->mName);
    }
    for (const otSrpClientService *service = services; service; service = service->mNext) {
      ESP_LOGW(TAG, "  Service: %s", service->mName);
    }
  }
}

void OpenThreadSrpComponent::srp_start_callback(const otSockAddr *server_socket_address, void *context) {
  ESP_LOGI(TAG, "SRP client has started");
}

void OpenThreadSrpComponent::srp_factory_reset_callback(otError err, const otSrpClientHostInfo *host_info,
                                                        const otSrpClientService *services,
                                                        const otSrpClientService *removed_services, void *context) {
  OpenThreadComponent *obj = (OpenThreadComponent *) context;
  if (err == OT_ERROR_NONE && removed_services != NULL && host_info != NULL &&
      host_info->mState == OT_SRP_CLIENT_ITEM_STATE_REMOVED) {
    ESP_LOGD(TAG, "Successful Removal SRP Host and Services");
  } else if (err != OT_ERROR_NONE) {
    // Handle other SRP client events or errors
    ESP_LOGW(TAG, "SRP client event/error: %s", otThreadErrorToString(err));
  }
  obj->defer_factory_reset_external_callback();
}

void OpenThreadSrpComponent::setup() {
  otError error;
  InstanceLock lock = InstanceLock::acquire();
  otInstance *instance = lock.get_instance();

  otSrpClientSetCallback(instance, OpenThreadSrpComponent::srp_callback, nullptr);

  // set the host name
  uint16_t size;
  char *existing_host_name = otSrpClientBuffersGetHostNameString(instance, &size);
  const std::string &host_name = App.get_name();
  uint16_t host_name_len = host_name.size();
  if (host_name_len > size) {
    ESP_LOGW(TAG, "Hostname is too long, choose a shorter project name");
    return;
  }
  memset(existing_host_name, 0, size);
  memcpy(existing_host_name, host_name.c_str(), host_name_len);

  error = otSrpClientSetHostName(instance, existing_host_name);
  if (error != 0) {
    ESP_LOGW(TAG, "Could not set host name");
    return;
  }

  error = otSrpClientEnableAutoHostAddress(instance);
  if (error != 0) {
    ESP_LOGW(TAG, "Could not enable auto host address");
    return;
  }

  // Get mdns services and copy their data (strings are copied with strdup below)
  const auto &mdns_services = this->mdns_->get_services();
  ESP_LOGD(TAG, "Setting up SRP services. count = %d\n", mdns_services.size());
  for (const auto &service : mdns_services) {
    otSrpClientBuffersServiceEntry *entry = otSrpClientBuffersAllocateService(instance);
    if (!entry) {
      ESP_LOGW(TAG, "Failed to allocate service entry");
      continue;
    }

    // Set service name
    char *string = otSrpClientBuffersGetServiceEntryServiceNameString(entry, &size);
    std::string full_service = std::string(MDNS_STR_ARG(service.service_type)) + "." + MDNS_STR_ARG(service.proto);
    if (full_service.size() > size) {
      ESP_LOGW(TAG, "Service name too long: %s", full_service.c_str());
      continue;
    }
    memcpy(string, full_service.c_str(), full_service.size() + 1);

    // Set instance name (using host_name)
    string = otSrpClientBuffersGetServiceEntryInstanceNameString(entry, &size);
    if (host_name_len > size) {
      ESP_LOGW(TAG, "Instance name too long: %s", host_name.c_str());
      continue;
    }
    memset(string, 0, size);
    memcpy(string, host_name.c_str(), host_name_len);

    // Set port
    entry->mService.mPort = const_cast<TemplatableValue<uint16_t> &>(service.port).value();

    otDnsTxtEntry *txt_entries =
        reinterpret_cast<otDnsTxtEntry *>(this->pool_alloc_(sizeof(otDnsTxtEntry) * service.txt_records.size()));
    // Set TXT records
    entry->mService.mNumTxtEntries = service.txt_records.size();
    for (size_t i = 0; i < service.txt_records.size(); i++) {
      const auto &txt = service.txt_records[i];
      // Value is either a compile-time string literal in flash or a pointer to dynamic_txt_values_
      // OpenThread SRP client expects the data to persist, so we strdup it
      const char *value_str = MDNS_STR_ARG(txt.value);
      txt_entries[i].mKey = MDNS_STR_ARG(txt.key);
      txt_entries[i].mValue = reinterpret_cast<const uint8_t *>(strdup(value_str));
      txt_entries[i].mValueLength = strlen(value_str);
    }
    entry->mService.mTxtEntries = txt_entries;
    entry->mService.mNumTxtEntries = service.txt_records.size();

    // Add service
    error = otSrpClientAddService(instance, &entry->mService);
    if (error != OT_ERROR_NONE) {
      ESP_LOGW(TAG, "Failed to add service: %s", otThreadErrorToString(error));
    }
    ESP_LOGD(TAG, "Added service: %s", full_service.c_str());
  }

  otSrpClientEnableAutoStartMode(instance, OpenThreadSrpComponent::srp_start_callback, nullptr);
  ESP_LOGD(TAG, "Finished SRP setup");
}

void *OpenThreadSrpComponent::pool_alloc_(size_t size) {
  uint8_t *ptr = new uint8_t[size];
  this->memory_pool_.emplace_back(std::unique_ptr<uint8_t[]>(ptr));
  return ptr;
}

void OpenThreadSrpComponent::set_mdns(esphome::mdns::MDNSComponent *mdns) { this->mdns_ = mdns; }

bool OpenThreadComponent::teardown() {
  if (!this->teardown_started_) {
    this->teardown_started_ = true;
    ESP_LOGD(TAG, "Clear Srp");
    auto lock = InstanceLock::try_acquire(100);
    if (!lock) {
      ESP_LOGW(TAG, "Failed to acquire OpenThread lock during teardown, leaking memory");
      return true;
    }
    otInstance *instance = lock->get_instance();
    otSrpClientClearHostAndServices(instance);
    otSrpClientBuffersFreeAllServices(instance);
    global_openthread_component = nullptr;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    ESP_LOGD(TAG, "Exit main loop ");
    int error = esp_openthread_mainloop_exit();
    if (error != ESP_OK) {
      ESP_LOGW(TAG, "Failed attempt to stop main loop %d", error);
      this->teardown_complete_ = true;
    }
#else
    this->teardown_complete_ = true;
#endif
  }
  return this->teardown_complete_;
}

void OpenThreadComponent::on_factory_reset(std::function<void()> callback) {
  factory_reset_external_callback_ = callback;
  ESP_LOGD(TAG, "Start Removal SRP Host and Services");
  otError error;
  InstanceLock lock = InstanceLock::acquire();
  otInstance *instance = lock.get_instance();
  otSrpClientSetCallback(instance, OpenThreadSrpComponent::srp_factory_reset_callback, this);
  error = otSrpClientRemoveHostAndServices(instance, true, true);
  if (error != OT_ERROR_NONE) {
    ESP_LOGW(TAG, "Failed to Remove SRP Host and Services");
    return;
  }
  ESP_LOGD(TAG, "Waiting on Confirmation Removal SRP Host and Services");
}

// set_use_address() is guaranteed to be called during component setup by Python code generation,
// so use_address_ will always be valid when get_use_address() is called - no fallback needed.
const char *OpenThreadComponent::get_use_address() const { return this->use_address_; }

void OpenThreadComponent::set_use_address(const char *use_address) { this->use_address_ = use_address; }

// Runtime provisioning API implementations
bool OpenThreadComponent::set_dataset_tlv(const std::vector<uint8_t> &tlv_data) {
  auto lock = InstanceLock::try_acquire(100);
  if (!lock) {
    ESP_LOGW(TAG, "Failed to acquire OpenThread lock in set_dataset_tlv");
    return false;
  }

  otInstance *instance = lock->get_instance();
  if (instance == nullptr) {
    ESP_LOGE(TAG, "OpenThread instance is null");
    return false;
  }

  otOperationalDatasetTlvs dataset_tlvs;
  if (tlv_data.size() > sizeof(dataset_tlvs.mTlvs)) {
    ESP_LOGE(TAG, "Dataset TLV too large: %zu bytes (max %zu)", tlv_data.size(), sizeof(dataset_tlvs.mTlvs));
    return false;
  }

  dataset_tlvs.mLength = tlv_data.size();
  memcpy(dataset_tlvs.mTlvs, tlv_data.data(), tlv_data.size());

  otError error = otDatasetSetActiveTlvs(instance, &dataset_tlvs);
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "Failed to set active dataset TLVs: %s", otThreadErrorToString(error));
    return false;
  }

  this->dataset_configured_ = true;
  ESP_LOGD(TAG, "Active dataset TLVs set successfully");
  return true;
}

bool OpenThreadComponent::set_dataset_params(const std::string &network_name, uint16_t pan_id, uint64_t ext_pan_id,
                                             const std::vector<uint8_t> &network_key, uint8_t channel,
                                             const std::vector<uint8_t> &pskc) {
  auto lock = InstanceLock::try_acquire(100);
  if (!lock) {
    ESP_LOGW(TAG, "Failed to acquire OpenThread lock in set_dataset_params");
    return false;
  }

  otInstance *instance = lock->get_instance();
  if (instance == nullptr) {
    ESP_LOGE(TAG, "OpenThread instance is null");
    return false;
  }

  otOperationalDataset dataset;
  memset(&dataset, 0, sizeof(dataset));

  // Set network name
  if (network_name.length() > OT_NETWORK_NAME_MAX_SIZE) {
    ESP_LOGE(TAG, "Network name too long: %zu chars (max %d)", network_name.length(), OT_NETWORK_NAME_MAX_SIZE);
    return false;
  }
  size_t max_name_len = sizeof(dataset.mNetworkName.m8) - 1;
  size_t name_len = std::min(network_name.length(), max_name_len);
  memcpy(dataset.mNetworkName.m8, network_name.c_str(), name_len);
  if (name_len < sizeof(dataset.mNetworkName.m8)) {
    dataset.mNetworkName.m8[name_len] = '\0';  // Ensure null-termination
  }
  dataset.mComponents.mIsNetworkNamePresent = true;

  // Set PAN ID
  dataset.mPanId = pan_id;
  dataset.mComponents.mIsPanIdPresent = true;

  // Set Extended PAN ID (network byte order - big-endian)
  for (int i = 0; i < 8; i++) {
    dataset.mExtendedPanId.m8[i] = (ext_pan_id >> (56 - i * 8)) & 0xFF;
  }
  dataset.mComponents.mIsExtendedPanIdPresent = true;

  // Set Network Key
  if (network_key.size() != OT_NETWORK_KEY_SIZE) {
    ESP_LOGE(TAG, "Invalid network key size: %zu bytes (expected %d)", network_key.size(), OT_NETWORK_KEY_SIZE);
    return false;
  }
  memcpy(dataset.mNetworkKey.m8, network_key.data(), OT_NETWORK_KEY_SIZE);
  dataset.mComponents.mIsNetworkKeyPresent = true;

  // Set Channel (Thread channels are typically 11-26 for 2.4 GHz)
  if (channel < 11 || channel > 26) {
    ESP_LOGE(TAG, "Invalid channel: %u (valid range is 11-26)", channel);
    return false;
  }
  dataset.mChannel = channel;
  dataset.mComponents.mIsChannelPresent = true;

  // Set PSKc (optional)
  if (!pskc.empty()) {
    if (pskc.size() != OT_PSKC_MAX_SIZE) {
      ESP_LOGE(TAG, "Invalid PSKc size: %zu bytes (expected %d)", pskc.size(), OT_PSKC_MAX_SIZE);
      return false;
    }
    memcpy(dataset.mPskc.m8, pskc.data(), OT_PSKC_MAX_SIZE);
    dataset.mComponents.mIsPskcPresent = true;
  }

  otError error = otDatasetSetActive(instance, &dataset);
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "Failed to set active dataset: %s", otThreadErrorToString(error));
    return false;
  }

  this->dataset_configured_ = true;
  ESP_LOGD(TAG, "Active dataset set successfully");
  return true;
}

void OpenThreadComponent::clear_dataset() {
  auto lock = InstanceLock::try_acquire(100);
  if (!lock) {
    ESP_LOGW(TAG, "Failed to acquire OpenThread lock in clear_dataset");
    return;
  }

  otInstance *instance = lock->get_instance();
  if (instance == nullptr) {
    return;
  }

  otDatasetSetActive(instance, nullptr);
  this->dataset_configured_ = false;
  this->join_state_ = JoinState::NOT_JOINED;
  ESP_LOGD(TAG, "Dataset cleared");
}

bool OpenThreadComponent::has_dataset() { return this->dataset_configured_; }

bool OpenThreadComponent::save_dataset() {
  // Dataset persistence is handled automatically by OpenThread's non-volatile storage
  // This method exists for API compatibility but doesn't need to do anything explicit
  return this->dataset_configured_;
}

bool OpenThreadComponent::start_joining() {
  auto lock = InstanceLock::try_acquire(100);
  if (!lock) {
    ESP_LOGW(TAG, "Failed to acquire OpenThread lock in start_joining");
    return false;
  }

  otInstance *instance = lock->get_instance();
  if (instance == nullptr) {
    ESP_LOGE(TAG, "OpenThread instance is null");
    return false;
  }

  if (!this->dataset_configured_) {
    ESP_LOGE(TAG, "Cannot start joining: no dataset configured");
    return false;
  }

  // Enable Thread interface
  otError error = otIp6SetEnabled(instance, true);
  if (error != OT_ERROR_NONE && error != OT_ERROR_ALREADY) {
    ESP_LOGE(TAG, "Failed to enable IPv6: %s", otThreadErrorToString(error));
    return false;
  }

  // Start Thread protocol
  error = otThreadSetEnabled(instance, true);
  if (error != OT_ERROR_NONE && error != OT_ERROR_ALREADY) {
    ESP_LOGE(TAG, "Failed to start Thread: %s", otThreadErrorToString(error));
    return false;
  }

  this->join_state_ = JoinState::JOINING;
  if (this->join_callback_) {
    this->join_callback_(this->join_state_);
  }

  ESP_LOGD(TAG, "Thread joining started");
  return true;
}

OpenThreadComponent::JoinState OpenThreadComponent::get_join_state() {
  // Update join state based on current Thread role
  auto lock = InstanceLock::try_acquire(100);
  if (!lock) {
    return this->join_state_;
  }

  otInstance *instance = lock->get_instance();
  if (instance == nullptr) {
    return this->join_state_;
  }

  otDeviceRole role = otThreadGetDeviceRole(instance);
  
  if (role >= OT_DEVICE_ROLE_CHILD) {
    if (this->join_state_ != JoinState::JOINED) {
      this->join_state_ = JoinState::JOINED;
      if (this->join_callback_) {
        this->join_callback_(this->join_state_);
      }
    }
  } else if (role == OT_DEVICE_ROLE_DISABLED || role == OT_DEVICE_ROLE_DETACHED) {
    if (this->dataset_configured_ && this->join_state_ == JoinState::JOINING) {
      // Still trying to join
    } else if (!this->dataset_configured_) {
      this->join_state_ = JoinState::NOT_JOINED;
    }
  }

  return this->join_state_;
}

bool OpenThreadComponent::is_joined() {
  return this->get_join_state() == JoinState::JOINED;
}

void OpenThreadComponent::set_on_join_callback(std::function<void(JoinState)> callback) {
  this->join_callback_ = std::move(callback);
}

}  // namespace openthread
}  // namespace esphome

#endif
