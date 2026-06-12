import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv

from . import CONF_WEBER_CONNECT_BLE_ID, WeberConnectBLE, weber_connect_ble_ns

CONF_PAIR = "pair"
CONF_DISCONNECT = "disconnect"
CONF_USE_WITH_PHONE = "use_with_phone"
CONF_CLEAR_PAIRING_DATA = "clear_pairing_data"

WeberPairButton = weber_connect_ble_ns.class_("WeberPairButton", button.Button)
WeberDisconnectButton = weber_connect_ble_ns.class_(
    "WeberDisconnectButton", button.Button
)
WeberPhoneHandoffButton = weber_connect_ble_ns.class_(
    "WeberPhoneHandoffButton", button.Button
)
WeberClearPairingButton = weber_connect_ble_ns.class_(
    "WeberClearPairingButton", button.Button
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_WEBER_CONNECT_BLE_ID): cv.use_id(WeberConnectBLE),
            cv.Optional(CONF_PAIR): button.button_schema(WeberPairButton),
            cv.Optional(CONF_DISCONNECT): button.button_schema(
                WeberDisconnectButton
            ),
            cv.Optional(CONF_USE_WITH_PHONE): button.button_schema(
                WeberPhoneHandoffButton
            ),
            cv.Optional(CONF_CLEAR_PAIRING_DATA): button.button_schema(
                WeberClearPairingButton
            ),
        }
    ),
    cv.has_at_least_one_key(
        CONF_PAIR, CONF_DISCONNECT, CONF_USE_WITH_PHONE, CONF_CLEAR_PAIRING_DATA
    ),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_WEBER_CONNECT_BLE_ID])
    for key in (
        CONF_PAIR,
        CONF_DISCONNECT,
        CONF_USE_WITH_PHONE,
        CONF_CLEAR_PAIRING_DATA,
    ):
        if key in config:
            await button.new_button(config[key], parent)
