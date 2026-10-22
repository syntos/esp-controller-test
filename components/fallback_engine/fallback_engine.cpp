#include "fallback_engine.h"

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
static const uint32_t DEBOUNCE_MS = 30;
static const size_t PASS_MIN = 8, PASS_MAX = 64;

// Physical reset: hold the reset input for at least RESET_HOLD_MS,
// starting within RESET_WINDOW_MS after boot.
static const uint32_t RESET_WINDOW_MS = 60000;
static const uint32_t RESET_HOLD_MS = 10000;

static const char *const SETUP_PAGE = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Fallback setup</title>
<body style="font-family:sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem">
<h2>Set a password</h2>
<p>Choose a password (at least 8 characters) for this page. User name: <b>admin</b>.</p>
<p>The password travels unencrypted over your local network, so do not reuse a password from elsewhere.</p>
<input type=password id=p minlength=8 autocomplete=new-password>
<button onclick=go()>Save</button>
<pre id=o></pre>
<script>
async function go(){
let r=await fetch('/fallback/setup',{method:'POST',headers:{'Content-Type':'text/plain'},body:p.value});
if(r.ok){location.reload()}else{o.textContent=await r.text()}}
</script>)HTML";

static const char *const PAGE = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Fallback</title>
<body style="font-family:sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem">
<h2>Fallback configuration</h2>
<p id=s>...</p>
<input type=file id=f accept=".json,application/json">
<button onclick=up()>Upload</button>
<p><a href=/fallback/config download=fallback.json>Download current configuration</a></p>
<button onclick=clr()>Clear configuration</button>
<pre id=o></pre>
<script>
async function st(){let r=await fetch('/fallback/status');let j=await r.json();
s.textContent='Mode: '+(j.active?'FALLBACK':'normal')+' | Home Assistant: '+(j.ha?'connected':'away')+' | rules: '+j.rules}
async function up(){if(!f.files[0]){o.textContent='Choose a file first';return}
let t=await f.files[0].text();
let r=await fetch('/fallback',{method:'POST',headers:{'Content-Type':'text/plain'},body:t});
o.textContent=await r.text();st()}
async function clr(){if(!confirm('Clear the fallback configuration?'))return;
let r=await fetch('/fallback',{method:'DELETE'});o.textContent=await r.text();st()}
st()
</script>)HTML";

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
    // The first callback is the initial state after boot: ignore it.
    // Only a real press (rising edge) within the time window counts.
    if (!this->reset_seen_first_) {
      this->reset_seen_first_ = true;
      return;
    }
    if (state && millis() < RESET_WINDOW_MS) {
      this->reset_pressing_ = true;
      this->reset_press_start_ = millis();
    } else {
      this->reset_pressing_ = false;
    }
  });
}

void FallbackEngine::setup() {
  this->last_edge_.assign(this->inputs_.size(), 0);
  this->timer_active_.assign(this->outputs_.size(), false);
  this->timer_target_.assign(this->outputs_.size(), false);
  this->timer_end_.assign(this->outputs_.size(), 0);

  this->load_rules_();
  this->load_password_();
}

void FallbackEngine::dump_config() {
  ESP_LOGCONFIG(TAG, "Fallback engine:");
  ESP_LOGCONFIG(TAG, "  Inputs: %u, outputs: %u", (unsigned) this->inputs_.size(), (unsigned) this->outputs_.size());
  ESP_LOGCONFIG(TAG, "  Rules in NVS: %u", (unsigned) this->rules_.size());
  ESP_LOGCONFIG(TAG, "  Port: %u, password: %s", this->port_, this->pass_.empty() ? "NOT SET (setup pending)" : "set");
  ESP_LOGCONFIG(TAG, "  Disconnect delay: %ums, reconnect delay: %ums", (unsigned) this->disconnect_delay_,
                (unsigned) this->reconnect_delay_);
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
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    this->rules_.clear();
    this->cancel_timers_();
  }
  ESP_LOGW(TAG, "Fallback rules cleared");
}

// ---------------------------------------------------------------- JSON

bool FallbackEngine::parse_config_(const std::string &body, std::vector<Rule> &out, std::string &err) {
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
  s += "]}";
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
  std::string err;
  if (!this->parse_config_(body, parsed, err)) {
    return send_(req, "400 Bad Request", "text/plain", "Rejected: " + err);
  }
  if (!this->save_rules_(parsed)) {
    return send_(req, "500 Internal Server Error", "text/plain", "Saving to NVS failed");
  }
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    this->rules_ = parsed;
    this->cancel_timers_();
  }
  char buf[64];
  snprintf(buf, sizeof buf, "OK: %u rules saved", (unsigned) parsed.size());
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
  bool ha = api::global_api_server != nullptr && api::global_api_server->is_connected();
  size_t n;
  {
    std::lock_guard<std::mutex> lk(this->mutex_);
    n = this->rules_.size();
  }
  char buf[96];
  snprintf(buf, sizeof buf, "{\"active\":%s,\"ha\":%s,\"rules\":%u}", this->active_ ? "true" : "false",
           ha ? "true" : "false", (unsigned) n);
  return send_(req, "200 OK", "application/json", buf);
}

// ---------------------------------------------------------------- runtime

void FallbackEngine::loop() {
  if (!this->server_started_ && network::is_connected()) {
    this->server_started_ = true;  // single attempt
    this->start_server_();
  }

  if (this->reset_pressing_ && millis() - this->reset_press_start_ >= RESET_HOLD_MS) {
    this->reset_pressing_ = false;
    this->reset_password();
  }

  this->update_mode_();
  if (this->active_) this->process_timers_();
}

void FallbackEngine::update_mode_() {
  uint32_t now = millis();
  bool ha_up = api::global_api_server != nullptr && api::global_api_server->is_connected();
  if (ha_up != this->last_ha_up_) {
    this->last_ha_up_ = ha_up;
    this->last_change_ = now;
  }
  uint32_t stable = now - this->last_change_;
  if (!this->active_ && !ha_up && stable >= this->disconnect_delay_) {
    this->active_ = true;
    ESP_LOGW(TAG, "Home Assistant gone -> FALLBACK active (%u rules)", (unsigned) this->rules_.size());
  } else if (this->active_ && ha_up && stable >= this->reconnect_delay_) {
    this->active_ = false;
    {
      std::lock_guard<std::mutex> lk(this->mutex_);
      this->cancel_timers_();
    }
    ESP_LOGW(TAG, "Home Assistant back -> normal mode");
  }
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
