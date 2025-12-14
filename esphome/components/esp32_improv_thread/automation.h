#pragma once
#ifdef USE_ESP32
#ifdef USE_ESP32_IMPROV_THREAD_STATE_CALLBACK
#include "esp32_improv_thread_component.h"

#include "esphome/core/automation.h"

#include <improv.h>

namespace esphome {
namespace esp32_improv_thread {

class ESP32ImprovThreadProvisionedTrigger : public Trigger<> {
 public:
  explicit ESP32ImprovThreadProvisionedTrigger(ESP32ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_PROVISIONED && !parent->is_failed()) {
        trigger();
      }
    });
  }
};

class ESP32ImprovThreadProvisioningTrigger : public Trigger<> {
 public:
  explicit ESP32ImprovThreadProvisioningTrigger(ESP32ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_PROVISIONING && !parent->is_failed()) {
        trigger();
      }
    });
  }
};

class ESP32ImprovThreadStartTrigger : public Trigger<> {
 public:
  explicit ESP32ImprovThreadStartTrigger(ESP32ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if ((state == improv::STATE_AUTHORIZED || state == improv::STATE_AWAITING_AUTHORIZATION) &&
          !parent->is_failed()) {
        trigger();
      }
    });
  }
};

class ESP32ImprovThreadStateTrigger : public Trigger<improv::State, improv::Error> {
 public:
  explicit ESP32ImprovThreadStateTrigger(ESP32ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (!parent->is_failed()) {
        trigger(state, error);
      }
    });
  }
};

class ESP32ImprovThreadStoppedTrigger : public Trigger<> {
 public:
  explicit ESP32ImprovThreadStoppedTrigger(ESP32ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_STOPPED && !parent->is_failed()) {
        trigger();
      }
    });
  }
};

}  // namespace esp32_improv_thread
}  // namespace esphome
#endif
#endif
