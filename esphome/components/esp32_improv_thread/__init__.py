from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, esp32_ble, improv_base, output
from esphome.components.esp32 import (
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    only_on_variant,
)
from esphome.components.esp32_ble import BTLoggers
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ON_STATE, CONF_TRIGGER_ID

AUTO_LOAD = ["esp32_ble_server", "improv_base"]
CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = ["openthread", "esp32"]

CONF_AUTHORIZED_DURATION = "authorized_duration"
CONF_AUTHORIZER = "authorizer"
CONF_BLE_SERVER_ID = "ble_server_id"
CONF_IDENTIFY_DURATION = "identify_duration"
CONF_ON_PROVISIONED = "on_provisioned"
CONF_ON_PROVISIONING = "on_provisioning"
CONF_ON_START = "on_start"
CONF_ON_STOP = "on_stop"
CONF_STATUS_INDICATOR = "status_indicator"
CONF_THREAD_TIMEOUT = "thread_timeout"

# Default Thread timeout - longer than WiFi due to mesh joining
DEFAULT_THREAD_TIMEOUT = "60s"


improv_ns = cg.esphome_ns.namespace("improv")
Error = improv_ns.enum("Error")
State = improv_ns.enum("State")

esp32_improv_thread_ns = cg.esphome_ns.namespace("esp32_improv_thread")
ESP32ImprovThreadComponent = esp32_improv_thread_ns.class_(
    "ESP32ImprovThreadComponent", cg.Component
)
ESP32ImprovThreadProvisionedTrigger = esp32_improv_thread_ns.class_(
    "ESP32ImprovThreadProvisionedTrigger", automation.Trigger.template()
)
ESP32ImprovThreadProvisioningTrigger = esp32_improv_thread_ns.class_(
    "ESP32ImprovThreadProvisioningTrigger", automation.Trigger.template()
)
ESP32ImprovThreadStartTrigger = esp32_improv_thread_ns.class_(
    "ESP32ImprovThreadStartTrigger", automation.Trigger.template()
)
ESP32ImprovThreadStateTrigger = esp32_improv_thread_ns.class_(
    "ESP32ImprovThreadStateTrigger", automation.Trigger.template()
)
ESP32ImprovThreadStoppedTrigger = esp32_improv_thread_ns.class_(
    "ESP32ImprovThreadStoppedTrigger", automation.Trigger.template()
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ESP32ImprovThreadComponent),
            cv.Required(CONF_AUTHORIZER): cv.Any(
                cv.none, cv.use_id(binary_sensor.BinarySensor)
            ),
            cv.Optional(CONF_STATUS_INDICATOR): cv.use_id(output.BinaryOutput),
            cv.Optional(
                CONF_IDENTIFY_DURATION, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_AUTHORIZED_DURATION, default="1min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_THREAD_TIMEOUT, default=DEFAULT_THREAD_TIMEOUT
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_PROVISIONED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        ESP32ImprovThreadProvisionedTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_PROVISIONING): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        ESP32ImprovThreadProvisioningTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_START): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        ESP32ImprovThreadStartTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_STATE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        ESP32ImprovThreadStateTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_STOP): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        ESP32ImprovThreadStoppedTrigger
                    ),
                }
            ),
        }
    )
    .extend(improv_base.IMPROV_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32C5, VARIANT_ESP32C6, VARIANT_ESP32H2]),
    cv.only_with_esp_idf,
)


async def to_code(config):
    # Register the loggers this component needs
    esp32_ble.register_bt_logger(BTLoggers.GATT, BTLoggers.SMP)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_define("USE_IMPROV_THREAD")

    await improv_base.setup_improv_core(var, config, "esp32_improv_thread")

    cg.add(var.set_identify_duration(config[CONF_IDENTIFY_DURATION]))
    cg.add(var.set_authorized_duration(config[CONF_AUTHORIZED_DURATION]))

    cg.add(var.set_thread_timeout(config[CONF_THREAD_TIMEOUT]))

    if CONF_AUTHORIZER in config and config[CONF_AUTHORIZER] is not None:
        activator = await cg.get_variable(config[CONF_AUTHORIZER])
        cg.add(var.set_authorizer(activator))

    if CONF_STATUS_INDICATOR in config:
        status_indicator = await cg.get_variable(config[CONF_STATUS_INDICATOR])
        cg.add(var.set_status_indicator(status_indicator))

    use_state_callback = False
    for conf in config.get(CONF_ON_PROVISIONED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_PROVISIONING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_STATE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(State, "state"), (Error, "error")], conf
        )
        use_state_callback = True
    for conf in config.get(CONF_ON_STOP, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        use_state_callback = True
    if use_state_callback:
        cg.add_define("USE_ESP32_IMPROV_THREAD_STATE_CALLBACK")
