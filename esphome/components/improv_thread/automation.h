#pragma once

#include "esphome/core/automation.h"
#include "improv_thread_component.h"

namespace esphome {
namespace improv_thread {

class ImprovThreadProvisionedTrigger : public Trigger<> {
 public:
  explicit ImprovThreadProvisionedTrigger(ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_PROVISIONED && !parent->is_failed()) {
        this->trigger();
      }
    });
  }
};

class ImprovThreadProvisioningTrigger : public Trigger<> {
 public:
  explicit ImprovThreadProvisioningTrigger(ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_PROVISIONING && !parent->is_failed()) {
        this->trigger();
      }
    });
  }
};

class ImprovThreadStartTrigger : public Trigger<> {
 public:
  explicit ImprovThreadStartTrigger(ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if ((state == improv::STATE_AUTHORIZED || state == improv::STATE_AWAITING_AUTHORIZATION) &&
          !parent->is_failed()) {
        this->trigger();
      }
    });
  }
};

class ImprovThreadStateTrigger : public Trigger<improv::State, improv::Error> {
 public:
  explicit ImprovThreadStateTrigger(ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      this->trigger(state, error);
    });
  }
};

class ImprovThreadStoppedTrigger : public Trigger<> {
 public:
  explicit ImprovThreadStoppedTrigger(ImprovThreadComponent *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_STOPPED && !parent->is_failed()) {
        this->trigger();
      }
    });
  }
};

}  // namespace improv_thread
}  // namespace esphome
