import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from . import CONF_WEBER_CONNECT_BLE_ID, WeberConnectBLE

CONF_PROBE = "probe"
CONF_STATE = "state"
CONF_CONNECTION_STATE = "connection_state"
CONF_HUB_SERIAL_OR_DEVICE_ID = "hub_serial_or_device_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_WEBER_CONNECT_BLE_ID): cv.use_id(WeberConnectBLE),
        cv.Optional(CONF_PROBE): cv.int_range(min=1, max=4),
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_CONNECTION_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_HUB_SERIAL_OR_DEVICE_ID): text_sensor.text_sensor_schema(),
    }
)


def _validate_text_sensor(config):
    if CONF_STATE in config and CONF_PROBE not in config:
        raise cv.Invalid("probe is required when state is configured")
    if not any(
        key in config
        for key in (CONF_STATE, CONF_CONNECTION_STATE, CONF_HUB_SERIAL_OR_DEVICE_ID)
    ):
        raise cv.Invalid("configure at least one Weber text sensor")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _validate_text_sensor)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_WEBER_CONNECT_BLE_ID])
    if CONF_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATE])
        cg.add(parent.set_probe_state_sensor(config[CONF_PROBE], sens))
    if CONF_CONNECTION_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CONNECTION_STATE])
        cg.add(parent.set_connection_state_sensor(sens))
    if CONF_HUB_SERIAL_OR_DEVICE_ID in config:
        sens = await text_sensor.new_text_sensor(config[CONF_HUB_SERIAL_OR_DEVICE_ID])
        cg.add(parent.set_hub_device_id_sensor(sens))
