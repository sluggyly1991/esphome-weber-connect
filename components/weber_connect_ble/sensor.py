import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_TEMPERATURE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
)

from . import CONF_WEBER_CONNECT_BLE_ID, WeberConnectBLE

CONF_PROBE = "probe"
CONF_BATTERY = "battery"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_WEBER_CONNECT_BLE_ID): cv.use_id(WeberConnectBLE),
            cv.Required(CONF_PROBE): cv.int_range(min=1, max=4),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_BATTERY): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    ),
    cv.has_at_least_one_key(CONF_TEMPERATURE, CONF_BATTERY),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_WEBER_CONNECT_BLE_ID])
    probe = config[CONF_PROBE]
    if CONF_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_TEMPERATURE])
        cg.add(parent.set_probe_temperature_sensor(probe, sens))
    if CONF_BATTERY in config:
        sens = await sensor.new_sensor(config[CONF_BATTERY])
        cg.add(parent.set_probe_battery_sensor(probe, sens))
