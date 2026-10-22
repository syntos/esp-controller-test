import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, light
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["network", "api", "esp32"]
AUTO_LOAD = ["json"]

fallback_ns = cg.esphome_ns.namespace("fallback_engine")
FallbackEngine = fallback_ns.class_("FallbackEngine", cg.Component)

CONF_INPUTS = "inputs"
CONF_OUTPUTS = "outputs"
CONF_DISCONNECT_DELAY = "disconnect_delay"
CONF_RECONNECT_DELAY = "reconnect_delay"
CONF_RESET_INPUT = "reset_input"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FallbackEngine),
        # Volgorde = indexnummer in het fallback-bestand (eerste = ingang/uitgang 0)
        cv.Required(CONF_INPUTS): cv.All(
            cv.ensure_list(cv.use_id(binary_sensor.BinarySensor)),
            cv.Length(min=1, max=255),
        ),
        cv.Required(CONF_OUTPUTS): cv.All(
            cv.ensure_list(cv.use_id(light.LightState)),
            cv.Length(min=1, max=255),
        ),
        cv.Optional(CONF_DISCONNECT_DELAY, default="15s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RECONNECT_DELAY, default="10s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PORT, default=80): cv.port,
        # Optioneel: een drukknop-ingang waarmee de klant het wachtwoord kan resetten
        cv.Optional(CONF_RESET_INPUT): cv.use_id(binary_sensor.BinarySensor),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for sid in config[CONF_INPUTS]:
        sens = await cg.get_variable(sid)
        cg.add(var.add_input(sens))
    for lid in config[CONF_OUTPUTS]:
        out = await cg.get_variable(lid)
        cg.add(var.add_output(out))

    if CONF_RESET_INPUT in config:
        rst = await cg.get_variable(config[CONF_RESET_INPUT])
        cg.add(var.set_reset_input(rst))

    cg.add(var.set_disconnect_delay(config[CONF_DISCONNECT_DELAY]))
    cg.add(var.set_reconnect_delay(config[CONF_RECONNECT_DELAY]))
    cg.add(var.set_port(config[CONF_PORT]))

    # ESPHome sluit ongebruikte ESP-IDF-onderdelen uit; de HTTP-server hebben we nodig.
    include_builtin_idf_component("esp_http_server")