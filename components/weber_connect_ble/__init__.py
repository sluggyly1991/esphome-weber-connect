import esphome.codegen as cg
from esphome.components import ble_client
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME

CODEOWNERS = []
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor", "text_sensor", "button"]

CONF_POLL_INTERVAL = "poll_interval"
CONF_LISTEN_DURATION = "listen_duration"
CONF_PHONE_HANDOFF_DURATION = "phone_handoff_duration"
CONF_PAIRING_NAME = "pairing_name"
CONF_HANDSHAKE_CHARACTERISTIC = "handshake_characteristic"
CONF_PAIRING_CHARACTERISTIC = "pairing_characteristic"
CONF_WEBER_CONNECT_BLE_ID = "weber_connect_ble_id"

weber_connect_ble_ns = cg.esphome_ns.namespace("weber_connect_ble")
WeberConnectBLE = weber_connect_ble_ns.class_(
    "WeberConnectBLE", cg.Component, ble_client.BLEClientNode
)
CharacteristicTarget = weber_connect_ble_ns.enum("CharacteristicTarget")

CHARACTERISTIC_TARGETS = {
    "session": CharacteristicTarget.SESSION,
    "command": CharacteristicTarget.COMMAND,
}


def _validate_timing(config):
    if config[CONF_POLL_INTERVAL].total_milliseconds <= config[
        CONF_LISTEN_DURATION
    ].total_milliseconds:
        raise cv.Invalid("poll_interval must be longer than listen_duration")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WeberConnectBLE),
            cv.Optional(CONF_NAME, default="Weber Connect"): cv.string,
            cv.Optional(CONF_POLL_INTERVAL, default="30s"):
                cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LISTEN_DURATION, default="8s"):
                cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PHONE_HANDOFF_DURATION, default="15min"):
                cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PAIRING_NAME, default="ESPHome Weber"):
                cv.string_strict,
            cv.Optional(CONF_HANDSHAKE_CHARACTERISTIC, default="session"):
                cv.enum(CHARACTERISTIC_TARGETS, lower=True),
            cv.Optional(CONF_PAIRING_CHARACTERISTIC, default="command"):
                cv.enum(CHARACTERISTIC_TARGETS, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA),
    _validate_timing,
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_name(config[CONF_NAME]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL].total_milliseconds))
    cg.add(var.set_listen_duration(config[CONF_LISTEN_DURATION].total_milliseconds))
    cg.add(
        var.set_phone_handoff_duration(
            config[CONF_PHONE_HANDOFF_DURATION].total_milliseconds
        )
    )
    cg.add(var.set_pairing_name(config[CONF_PAIRING_NAME]))
    cg.add(
        var.set_handshake_characteristic(config[CONF_HANDSHAKE_CHARACTERISTIC])
    )
    cg.add(var.set_pairing_characteristic(config[CONF_PAIRING_CHARACTERISTIC]))
