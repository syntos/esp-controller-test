#include "fallback_engine.h"
#include "pages.h"

#include <cstring>

#include <nvs.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace fallback_engine {

static const char *const TAG = "fallback_engine";

static const char *const NVS_NS = "fallback";
static const char *const USERNAME = "admin";
static const char *const TRIGGER_NAMES[] = {"both", "rising", "falling"};
static const char *const ACTION_NAMES[] = {"toggle", "on", "off", "on_for", "off_for"};
static const size_t MAX_RULES = 256;
static const size_t MAX_BODY = 32768;
static const size_t MAX_META = 16384;  // names / rule layout stored next to the rules
static const uint32_t DEBOUNCE_MS = 30;
static const size_t PASS_MIN = 8, PASS_MAX = 64;

// Physical reset: press the reset input within RESET_WINDOW_MS after boot and hold it for
// RESET_HOLD_MS. The status LED blinks white while holding and turns steady green when the
// password has been reset (release now).
static const uint32_t RESET_WINDOW_MS = 60000;
static const uint32_t BOOT_SETTLE_MS = 5000;  // initial input states are reported within this time
static const uint32_t RESET_HOLD_MS = 10000;

// The web pages (SETUP_PAGE, PAGE) are generated into pages.h by tools/build_device_pages.py.

// ---------------------------------------------------------------- helpers

static uint32_t fnv1a(const uint8_t *d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= d[i];
    h *= 16777619u;
  }
  return h;
}

static int find_name(const char *const *names, int count, const char *s) {
  for (int i = 0; i < count; i++)
    if (strcmp(names[i], s) == 0) return i;
  return -1;
}

static std::string b64decode(const std::string &in) {
  static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    const char *p = c ? strchr(T, c) : nullptr;
    if (p == nullptr) return "";
    val = (val << 6) + (int) (p - T);
    bits += 6;
    if (bits >= 0) {
      out.push_back((char) ((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

static esp_err_t send_(httpd_req_t *req, const char *status, const char *type, const std::string &body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  return httpd_resp_send(req, body.c_str(), body.size());
}

static bool read_body_(httpd_req_t *req, std::string &out, size_t max) {
  size_t total = req->content_len;
  if (total == 0 || total > max) return false;
  out.assign(total, '\0');
  size_t got = 0;
  int timeouts = 0;
  while (got < total) {
    int r = httpd_req_recv(req, &out[got], total - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts > 5) return false;
      continue;
    }
    if (r <= 0) return false;
    got += r;
  }
  return true;
}

// ---------------------------------------------------------------- setup

void FallbackEngine::add_input(binary_sensor::BinarySensor *sensor) {
  uint8_t idx = this->inputs_.size();
  this->inputs_.push_back(sensor);
  // sensor is inverted: state==true means "active" (button pressed / switch on)
  sensor->add_on_state_callback([this, idx](bool state) { this->on_input_(idx, state); });
}

void FallbackEngine::set_reset_input(binary_sensor::BinarySensor *sensor) {
  sensor->add_on_state_callback([this](bool state) {
    // A button that is already held down while the controller boots does not count:
    // the first state reported right after boot is ignored if it is "pressed".
    if (!this->reset_seen_first_) {
      this->reset_seen_first_ = true;
      if (state && millis() < BOOT_SETTLE_MS) {
        ESP_LOGD(TAG, "Reset input held at boot (ignored until released)");
        return;
      }
    }
    if (state && millis() >= RESET_WINDOW_MS) {
      ESP_LOGI(TAG, "Reset input pressed after the %u s window: ignored (restart the controller to try again)",
               (unsigned) (RESET_WINDOW_MS / 1000));
      return;
    }
    if (state) {
      ESP_LOGI(TAG, "Reset input pressed: keep holding for %u s", (unsigned) (RESET_HOLD_MS / 1000));
      this->reset_pressing_ = true;
      this->reset_done_ = false;
      this->reset_press_start_ = millis();
      this->reset_blink_ = 0;
    } else {
      if (this->reset_pressing_) ESP_LOGI(TAG, "Reset input released");
      this->reset_pressing_ = false;
      this->reset_done_ = false;
      this->reset_led_restore_();
    }
  });
}

void FallbackEngine::setup() {
  this->last_edge_.assign(this->inputs_.size(), 0);
  this->timer_active_.assign(this->outputs_.size(), false);
  this->timer_target_.assign(this->outputs_.size(), false);
  this->timer_end_.assign(this->outputs_.size(), 0);

  this->load_rules_();
  this->load_meta_();
  this->load_password_();
}

void FallbackEngine::dump_config() {
  ESP_LOGCONFIG(TAG, "Fallback engine:");
  ESP_LOGCONFIG(TAG, "  Inputs: %u, outputs: %u", (unsigned) this->inputs_.size(), (unsigned) this->outputs_.size());
  ESP_LOGCONFIG(TAG, "  Rules in NVS: %u", (unsigned) this->rules_.size());
  ESP_LOGCONFIG(TAG, "  Port: %u, password: %s", this->port_, this->pass_.empty() ? "NOT SET (setup pending)" : "set");
  ESP_LOGCONFIG(TAG, "  Disconnect delay: %ums, reconnect delay: %ums, boot LED delay: %ums", (unsigned) this->disconnect_delay_,
                (unsigned) this->reconnect_delay_, (unsigned) this->boot_led_delay_);
}

// ---------------------------------------------------------------- NVS

void FallbackEngine::load_rules_() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "Could not open NVS namespace");
    return;
  }
  uint16_t n = 0;
  uint32_t hash = 0;
  nvs_get_u16(h, "count", &n);
  nvs_get_u32(h, "hash", &hash);
  if (n == 0 || n > MAX_RULES) {
    nvs_close(h);
    return;
  }
  std::vector<Rule> tmp(n);
  size_t bytes = n * sizeof(Rule);
  size_t len = bytes;
  esp_err_t e = nvs_get_blob(h, "rules", tmp.data(), &len);
  nvs_close(h);

  if (e != ESP_OK || len != bytes || fnv1a((const uint8_t *) tmp.data(), bytes) != hash) {
    ESP_LOGE(TAG, "Fallback config in NVS is corrupt, ignored");
    return;
  }
  this->rules_ = std::move(tmp);
  ESP_LOGI(TAG, "%u fallback rules loaded", (unsigned) this->rules_.size());
}

void FallbackEngine::load_meta_() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  size_t len = 0;
  if (nvs_get_blob(h, "meta", nullptr, &len) == ESP_OK && len > 2 && len <= MAX_META) {
    std::string s(len, '\0');
    if (nvs_get_blob(h, "meta", &s[0], &len) == ESP_OK) {
      s.resize(len);
      this->meta_ = s;
    }
  }
  nvs_close(h);
}

bool FallbackEngine::save_meta_(const std::string &meta) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = ESP_OK;
  if (meta.empty()) {
    nvs_erase_key(h, "meta");  // ESP_ERR_NVS_NOT_FOUND is fine
  } else {
    e = nvs_set_blob(h, "meta", meta.data(), meta.size());
  }
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

bool FallbackEngine::save_rules_(const std::vector<Rule> &rules) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = ESP_OK;
  if (rules.empty()) {
    nvs_erase_key(h, "rules");
    nvs_erase_key(h, "hash");
    nvs_erase_key(h, "count");
  } else {
    size_t bytes = rules.size() * sizeof(Rule);
    e = nvs_set_blob(h, "rules", rules.data(), bytes);
    if (e == ESP_OK) e = nvs_set_u32(h, "hash", fnv1a((const uint8_t *) rules.data(), bytes));
    if (e == ESP_OK) e = nvs_set_u16(h, "count", (uint16_t) rules.size());
  }
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

void FallbackEngine::load_password_() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  size_t len = 0;
  if (nvs_get_str(h, "pw", nullptr, &len) == ESP_OK && len > 1) {
    std::string s(len, '\0');
    if (nvs_get_str(h, "pw", &s[0], &len) == ESP_OK) {
      s.resize(len - 1);
      this->pass_ = s;
    }
  }
  nvs_close(h);
}

bool FallbackEngine::save_password_(const std::string &pw) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_str(h, "pw", pw.c_str());
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

std::string FallbackEngine::get_password_() {
  std::lock_guard<std::mutex> lk(this->mutex_);
  return this->pass_;
}

void FallbackEngine::reset_password() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "pw");
    nvs_commit(h);
    nvs_close(h);
  }
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    this->pass_.clear();
  }
  ESP_LOGW(TAG, "Password cleared: set a new password via /fallback");
}

void FallbackEngine::clear_rules() {
  std::vector<Rule> empty;
  this->save_rules_(empty);
  this->save_meta_("");
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    this->rules_.clear();
    this->meta_.clear();
    this->cancel_timers_();
  }
  ESP_LOGW(TAG, "Fallback rules cleared");
}

// ---------------------------------------------------------------- JSON

bool FallbackEngine::parse_config_(const std::string &body, std::vector<Rule> &out, std::string &meta, std::string &err) {
  err = "invalid JSON";
  bool ok = json::parse_json(body, [&](JsonObject root) -> bool {
    if ((int) (root["version"] | 0) != 1) {
      err = "version must be 1";
      return false;
    }
    JsonArray arr = root["rules"].as<JsonArray>();
    if (arr.isNull()) {
      err = "'rules' is missing";
      return false;
    }
    if (arr.size() > MAX_RULES) {
      err = "too many rules";
      return false;
    }
    size_t line = 0;
    for (JsonObject r : arr) {
      line++;
      int in = r["in"] | -1;
      int o = r["out"] | -1;
      const char *t = r["trigger"] | "";
      const char *a = r["action"] | "";
      int sec = r["seconds"] | 0;
      int ti = find_name(TRIGGER_NAMES, 3, t);
      int ai = find_name(ACTION_NAMES, 5, a);
      char buf[48];
      if (in < 0 || in >= (int) this->inputs_.size()) {
        snprintf(buf, sizeof buf, "rule %u: invalid input", (unsigned) line);
        err = buf;
        return false;
      }
      if (o < 0 || o >= (int) this->outputs_.size()) {
        snprintf(buf, sizeof buf, "rule %u: invalid output", (unsigned) line);
        err = buf;
        return false;
      }
      if (ti < 0 || ai < 0) {
        snprintf(buf, sizeof buf, "rule %u: invalid trigger/action", (unsigned) line);
        err = buf;
        return false;
      }
      if ((ai == ACT_ON_FOR || ai == ACT_OFF_FOR) && (sec < 1 || sec > 65535)) {
        snprintf(buf, sizeof buf, "rule %u: seconds must be 1-65535", (unsigned) line);
        err = buf;
        return false;
      }
      Rule rule;
      rule.in = in;
      rule.out = o;
      rule.trigger = ti;
      rule.action = ai;
      rule.seconds = (ai == ACT_ON_FOR || ai == ACT_OFF_FOR) ? sec : 0;
      out.push_back(rule);
    }
    // Keep everything else in the file (names, groups, ...) verbatim, so it can be downloaded again.
    root.remove("version");
    root.remove("rules");
    if (root.size() > 0) serializeJson(root, meta);
    return true;
  });
  return ok;
}

std::string FallbackEngine::rules_to_json_() {
  std::lock_guard<std::mutex> lk(this->mutex_);
  std::string s = "{\"version\":1,\"rules\":[";
  char buf[160];
  for (size_t i = 0; i < this->rules_.size(); i++) {
    const Rule &r = this->rules_[i];
    char sec[24] = "";
    if (r.action == ACT_ON_FOR || r.action == ACT_OFF_FOR) snprintf(sec, sizeof sec, ",\"seconds\":%u", r.seconds);
    snprintf(buf, sizeof buf, "%s{\"in\":%u,\"trigger\":\"%s\",\"out\":%u,\"action\":\"%s\"%s}", i ? "," : "", r.in,
             TRIGGER_NAMES[r.trigger % 3], r.out, ACTION_NAMES[r.action % 5], sec);
    s += buf;
  }
  s += "]";
  if (this->meta_.size() > 2) {  // meta_ is a JSON object: splice its members in
    s += ",";
    s.append(this->meta_, 1, this->meta_.size() - 2);
  }
  s += "}";
  return s;
}

// ---------------------------------------------------------------- HTTP

void FallbackEngine::start_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->port_;
  cfg.ctrl_port = 32770;  // differs from the default, in case ESPHome's web_server also runs
  cfg.max_uri_handlers = 12;
  cfg.stack_size = 10240;
  cfg.lru_purge_enable = true;

  if (httpd_start(&this->httpd_, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server (port %u in use?)", this->port_);
    this->httpd_ = nullptr;
    return;
  }

  auto reg = [this](const char *uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t *)) {
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = fn;
    u.user_ctx = this;
    httpd_register_uri_handler(this->httpd_, &u);
  };

  reg("/fallback", HTTP_GET, [](httpd_req_t *r) -> esp_err_t {
    return static_cast<FallbackEngine *>(r->user_ctx)->on_page_(r);
  });
  reg("/fallback/setup", HTTP_POST, [](httpd_req_t *r) -> esp_err_t {
    return static_cast<FallbackEngine *>(r->user_ctx)->on_setup_(r);
  });
  reg("/fallback", HTTP_POST, [](httpd_req_t *r) -> esp_err_t {
    return static_cast<FallbackEngine *>(r->user_ctx)->on_upload_(r);
  });
  reg("/fallback", HTTP_DELETE, [](httpd_req_t *r) -> esp_err_t {
    return static_cast<FallbackEngine *>(r->user_ctx)->on_delete_(r);
  });
  reg("/fallback/config", HTTP_GET, [](httpd_req_t *r) -> esp_err_t {
    return static_cast<FallbackEngine *>(r->user_ctx)->on_config_(r);
  });
  reg("/fallback/status", HTTP_GET, [](httpd_req_t *r) -> esp_err_t {
    return static_cast<FallbackEngine *>(r->user_ctx)->on_status_(r);
  });

  ESP_LOGI(TAG, "Upload page active at /fallback (port %u)", this->port_);
}

bool FallbackEngine::check_auth_(httpd_req_t *req) {
  std::string pw = this->get_password_();
  if (pw.empty()) {
    send_(req, "403 Forbidden", "text/plain", "Set a password first via /fallback");
    return false;
  }
  size_t hl = httpd_req_get_hdr_value_len(req, "Authorization");
  if (hl > 0 && hl < 256) {
    std::string hdr(hl + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Authorization", &hdr[0], hl + 1) == ESP_OK) {
      hdr.resize(hl);
      if (hdr.rfind("Basic ", 0) == 0) {
        std::string dec = b64decode(hdr.substr(6));
        if (dec == std::string(USERNAME) + ":" + pw) return true;
      }
    }
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Fallback\"");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "Login required", HTTPD_RESP_USE_STRLEN);
  return false;
}

esp_err_t FallbackEngine::on_page_(httpd_req_t *req) {
  if (this->get_password_().empty()) {
    return send_(req, "200 OK", "text/html", SETUP_PAGE);  // first use: choose a password
  }
  if (!this->check_auth_(req)) return ESP_OK;
  return send_(req, "200 OK", "text/html", PAGE);
}

esp_err_t FallbackEngine::on_setup_(httpd_req_t *req) {
  if (!this->get_password_().empty()) {
    return send_(req, "403 Forbidden", "text/plain", "Password is already set");
  }
  std::string body;
  if (!read_body_(req, body, 256) || body.size() < PASS_MIN || body.size() > PASS_MAX) {
    return send_(req, "400 Bad Request", "text/plain", "The password must be 8 to 64 characters long");
  }
  for (unsigned char c : body) {
    if (c < 0x20 || c == 0x7f) {
      return send_(req, "400 Bad Request", "text/plain", "Invalid characters in password");
    }
  }
  if (!this->save_password_(body)) {
    return send_(req, "500 Internal Server Error", "text/plain", "Saving failed");
  }
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    this->pass_ = body;
  }
  ESP_LOGI(TAG, "Password set");
  return send_(req, "200 OK", "text/plain", "OK");
}

esp_err_t FallbackEngine::on_upload_(httpd_req_t *req) {
  if (!this->check_auth_(req)) return ESP_OK;
  std::string body;
  if (!read_body_(req, body, MAX_BODY)) {
    return send_(req, "400 Bad Request", "text/plain", "Empty or too large upload");
  }
  std::vector<Rule> parsed;
  std::string err, meta;
  if (!this->parse_config_(body, parsed, meta, err)) {
    return send_(req, "400 Bad Request", "text/plain", "Rejected: " + err);
  }
  if (!this->save_rules_(parsed)) {
    return send_(req, "500 Internal Server Error", "text/plain", "Saving to NVS failed");
  }
  bool meta_dropped = false;
  if (meta.size() > MAX_META) {
    meta.clear();
    meta_dropped = true;
  }
  if (!this->save_meta_(meta)) meta.clear();
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    this->rules_ = parsed;
    this->meta_ = meta;
    this->cancel_timers_();
  }
  char buf[128];
  snprintf(buf, sizeof buf, "OK: %u rules saved%s", (unsigned) parsed.size(),
           meta_dropped ? " (names and layout were too large and not stored)" : "");
  ESP_LOGI(TAG, "%s", buf);
  return send_(req, "200 OK", "text/plain", buf);
}

esp_err_t FallbackEngine::on_delete_(httpd_req_t *req) {
  if (!this->check_auth_(req)) return ESP_OK;
  this->clear_rules();
  return send_(req, "200 OK", "text/plain", "Configuration cleared");
}

esp_err_t FallbackEngine::on_config_(httpd_req_t *req) {
  if (!this->check_auth_(req)) return ESP_OK;
  return send_(req, "200 OK", "application/json", this->rules_to_json_());
}

esp_err_t FallbackEngine::on_status_(httpd_req_t *req) {
  if (!this->check_auth_(req)) return ESP_OK;
  bool ha = ha_connected_();
  size_t n;
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    n = this->rules_.size();
  }
  std::string out = "{\"active\":";
  out += this->active_ ? "true" : "false";
  out += ",\"ha\":";
  out += ha ? "true" : "false";
  out += ",\"rules\":" + std::to_string(n);
  if (!this->editor_url_.empty()) out += ",\"editor\":\"" + this->editor_url_ + "\"";  // validated as a plain URL in __init__.py
  out += "}";
  return send_(req, "200 OK", "application/json", out);
}

// ---------------------------------------------------------------- reset LED feedback

void FallbackEngine::reset_led_feedback_(bool on, float r, float g, float b) {
  if (this->status_light_ == nullptr) return;
  auto call = this->status_light_->make_call();
  call.set_state(on);
  if (on) {
    call.set_rgb(r, g, b);
    call.set_brightness(1.0f);
  }
  call.set_transition_length(0);
  call.perform();
  this->led_applied_ = -1;  // the connection colour is re-applied afterwards
}

void FallbackEngine::reset_led_restore_() {
  this->led_applied_ = -1;  // update_led_() paints the current connection colour again
}

// Only a client that subscribed to states is Home Assistant. Other API clients, such as the
// ESPHome Builder log viewer, must not count as "Home Assistant is connected".
bool FallbackEngine::ha_connected_() {
  return api::global_api_server != nullptr && api::global_api_server->is_connected_with_state_subscription();
}

void FallbackEngine::update_led_(bool ha_up) {
  if (this->status_light_ == nullptr || this->reset_pressing_) return;
  if (ha_up) this->seen_ha_ = true;
  int want;
  if (ha_up) want = 0;                  // blue
  else if (this->active_ && (this->seen_ha_ || millis() >= this->boot_led_delay_)) want = 1;  // orange: fallback mode
  else if (this->seen_ha_) want = 2;    // purple: connection lost, waiting
  else want = 3;                        // red: not connected since power-up
  if (want == this->led_applied_) return;
  this->led_applied_ = want;
  static const float COLORS[4][3] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.4f, 0.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}};
  auto call = this->status_light_->make_call();
  call.set_state(true);
  call.set_rgb(COLORS[want][0], COLORS[want][1], COLORS[want][2]);
  call.set_brightness(1.0f);
  call.set_transition_length(0);
  call.perform();
}

// ---------------------------------------------------------------- runtime

void FallbackEngine::loop() {
  if (!this->server_started_ && network::is_connected()) {
    this->server_started_ = true;  // single attempt
    this->start_server_();
  }

  if (this->reset_pressing_) {
    uint32_t now = millis();
    if (!this->reset_done_ && now - this->reset_press_start_ >= RESET_HOLD_MS) {
      this->reset_done_ = true;
      this->reset_password();
      this->reset_led_feedback_(true, 0.0f, 1.0f, 0.0f);  // steady green: you can release now
    } else if (!this->reset_done_ && now - this->reset_blink_ >= 250) {
      this->reset_blink_ = now;
      this->reset_blink_on_ = !this->reset_blink_on_;
      this->reset_led_feedback_(this->reset_blink_on_, 1.0f, 1.0f, 1.0f);  // blinking white
    }
  }

  this->update_mode_();
  if (this->active_) this->process_timers_();
}

void FallbackEngine::update_mode_() {
  uint32_t now = millis();
  bool ha_up = ha_connected_();
  if (ha_up != this->last_ha_up_) {
    this->last_ha_up_ = ha_up;
    this->last_change_ = now;
  }
  if (ha_up) this->seen_ha_ = true;
  uint32_t stable = now - this->last_change_;
  if (!this->active_ && !ha_up && stable >= this->disconnect_delay_) {
    this->active_ = true;
    ESP_LOGW(TAG, "Home Assistant %s -> FALLBACK active (%u rules)", this->seen_ha_ ? "gone" : "not connected since boot",
             (unsigned) this->rules_.size());
  } else if (this->active_ && ha_up && stable >= this->reconnect_delay_) {
    this->active_ = false;
    {
      std::lock_guard<std::mutex> lk(this->mutex_);
      this->cancel_timers_();
    }
    ESP_LOGW(TAG, "Home Assistant back -> normal mode");
  }
  this->update_led_(ha_up);
}

void FallbackEngine::on_input_(uint8_t idx, bool state) {
  if (!this->active_) return;
  uint32_t now = millis();
  if (now - this->last_edge_[idx] < DEBOUNCE_MS) return;
  this->last_edge_[idx] = now;

  std::lock_guard<std::mutex> lk(this->mutex_);
  for (const Rule &r : this->rules_) {
    if (r.in != idx) continue;
    if (r.trigger == TRG_RISING && !state) continue;
    if (r.trigger == TRG_FALLING && state) continue;
    this->run_action_(r);
  }
}

void FallbackEngine::run_action_(const Rule &r) {
  if (r.out >= this->outputs_.size()) return;
  uint8_t o = r.out;
  this->timer_active_[o] = false;  // every new action cancels a running timer
  switch (r.action) {
    case ACT_TOGGLE:
      this->set_out_(o, !this->get_out_(o));
      break;
    case ACT_ON:
      this->set_out_(o, true);
      break;
    case ACT_OFF:
      this->set_out_(o, false);
      break;
    case ACT_ON_FOR:
      this->set_out_(o, true);
      this->start_timer_(o, false, r.seconds);
      break;
    case ACT_OFF_FOR:
      this->set_out_(o, false);
      this->start_timer_(o, true, r.seconds);
      break;
  }
}

void FallbackEngine::start_timer_(uint8_t o, bool target, uint16_t seconds) {
  this->timer_active_[o] = true;
  this->timer_target_[o] = target;
  this->timer_end_[o] = millis() + (uint32_t) seconds * 1000UL;
}

void FallbackEngine::process_timers_() {
  uint32_t now = millis();
  std::lock_guard<std::mutex> lk(this->mutex_);
  for (size_t o = 0; o < this->timer_active_.size(); o++) {
    if (this->timer_active_[o] && (int32_t) (now - this->timer_end_[o]) >= 0) {
      this->timer_active_[o] = false;
      this->set_out_(o, this->timer_target_[o]);
    }
  }
}

void FallbackEngine::cancel_timers_() {
  for (size_t o = 0; o < this->timer_active_.size(); o++) this->timer_active_[o] = false;
}

bool FallbackEngine::get_out_(uint8_t o) { return this->outputs_[o]->remote_values.is_on(); }

void FallbackEngine::set_out_(uint8_t o, bool on) {
  // via the light entity, so the state syncs correctly to HA on reconnect
  auto call = this->outputs_[o]->make_call();
  call.set_state(on);
  call.set_transition_length(0);
  call.perform();
}

}  // namespace fallback_engine
}  // namespace esphome
