#pragma once

#include <memory>
#include <string>
#include <vector>

#include <Preferences.h>
#include <WebServer.h>

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/light/light_state.h"

namespace esphome {
namespace fallback_engine {

enum Trigger : uint8_t { TRG_BOTH = 0, TRG_RISING = 1, TRG_FALLING = 2 };
enum Action : uint8_t { ACT_TOGGLE = 0, ACT_ON = 1, ACT_OFF = 2, ACT_ON_FOR = 3, ACT_OFF_FOR = 4 };

// 6 bytes per regel in NVS
struct Rule {
  uint8_t in;
  uint8_t out;
  uint8_t trigger;
  uint8_t action;
  uint16_t seconds;
} __attribute__((packed));

class FallbackEngine : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void add_input(binary_sensor::BinarySensor *sensor);
  void add_output(light::LightState *light) { this->outputs_.push_back(light); }
  void set_reset_input(binary_sensor::BinarySensor *sensor);
  void set_disconnect_delay(uint32_t ms) { this->disconnect_delay_ = ms; }
  void set_reconnect_delay(uint32_t ms) { this->reconnect_delay_ = ms; }
  void set_port(uint16_t port) { this->port_ = port; }

  // Wist enkel het wachtwoord (de regels blijven bewaard). Aan te roepen vanuit een
  // ESPHome-knop: lambda: id(fallback).reset_password();
  void reset_password();

 protected:
  // regels
  void load_rules_();
  void save_rules_();
  bool parse_config_(const std::string &body, std::vector<Rule> &out, std::string &err);
  std::string rules_to_json_();

  // wachtwoord
  void load_password_();
  bool has_password_() const { return !this->pass_.empty(); }
  void handle_setup_();

  // webserver
  void register_routes_();
  bool auth_();
  void handle_upload_();

  // runtime
  void update_mode_();
  void on_input_(uint8_t idx, bool state);
  void run_action_(const Rule &r);
  void process_timers_();
  bool get_out_(uint8_t o);
  void set_out_(uint8_t o, bool on);
  void start_timer_(uint8_t o, bool target, uint16_t seconds);
  void cancel_timers_();

  std::vector<binary_sensor::BinarySensor *> inputs_;
  std::vector<light::LightState *> outputs_;
  std::vector<Rule> rules_;

  std::vector<uint32_t> last_edge_;
  std::vector<bool> timer_active_;
  std::vector<bool> timer_target_;
  std::vector<uint32_t> timer_end_;

  std::unique_ptr<WebServer> server_;
  bool server_started_{false};
  uint16_t port_{80};
  std::string pass_;

  // fysieke reset
  bool reset_seen_first_{false};
  bool reset_pressing_{false};
  uint32_t reset_press_start_{0};

  uint32_t disconnect_delay_{15000};
  uint32_t reconnect_delay_{10000};
  bool active_{false};
  bool last_ha_up_{false};
  uint32_t last_change_{0};
};

}  // namespace fallback_engine
}  // namespace esphome
