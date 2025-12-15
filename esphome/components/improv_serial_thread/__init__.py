import esphome.codegen as cg
from esphome.components import improv_base
from esphome.components.esp32 import (
    VARIANT_ESP32C5,
    VARIANT_ESP32C6,
    VARIANT_ESP32H2,
    only_on_variant,
)
from esphome.components.logger import USB_CDC
import esphome.config_validation as cv
from esphome.const import CONF_BAUD_RATE, CONF_HARDWARE_UART, CONF_ID, CONF_LOGGER
from esphome.core import CORE
import esphome.final_validate as fv

AUTO_LOAD = ["improv_base"]
CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = ["logger", "openthread"]

improv_serial_thread_ns = cg.esphome_ns.namespace("improv_serial_thread")

ImprovSerialThreadComponent = improv_serial_thread_ns.class_(
    "ImprovSerialThreadComponent", cg.Component
)

CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.GenerateID(): cv.declare_id(ImprovSerialThreadComponent)})
    .extend(improv_base.IMPROV_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32C5, VARIANT_ESP32C6, VARIANT_ESP32H2]),
    cv.only_with_esp_idf,
)


def validate_logger(config):
    logger_conf = fv.full_config.get()[CONF_LOGGER]
    if logger_conf[CONF_BAUD_RATE] == 0:
        raise cv.Invalid(
            "improv_serial_thread requires the logger baud_rate to be not 0"
        )
    return config


FINAL_VALIDATE_SCHEMA = validate_logger


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await improv_base.setup_improv_core(var, config, "improv_serial_thread")
