#include "fallback_engine.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace fallback_engine {

static const char *const TAG = "fallback_engine";

static const char *const USERNAME = "admin";
static const char *const TRIGGER_NAMES[] = {"both", "rising", "falling"};
static const char *const ACTION_NAMES[] = {"toggle", "on", "off", "on_for", "off_for"};
static const size_t MAX_RULES = 256;
static const uint32_t DEBOUNCE_MS = 30;
static const size_t PASS_MIN = 8, PASS_MAX = 64;

// Fysieke reset: reset-ingang minstens RESET_HOLD_MS ingedrukt houden,
// begonnen binnen RESET_WINDOW_MS na het opstarten.
static const uint32_t RESET_WINDOW_MS = 60000;
static const uint32_t RESET_HOLD_MS = 10000;

static const char *const SETUP_PAGE = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Fallback instellen</title>
<body style="font-family:sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem">
<h2>Wachtwoord instellen</h2>
<p>Kies een wachtwoord (minstens 8 tekens) voor deze pagina. Gebruikersnaam: <b>admin</b>.</p>
<p>Het wachtwoord reist onversleuteld over je lokale netwerk. Hergebruik dus geen wachtwoord van elders.</p>
<input type=password id=p minlength=8 autocomplete=new-password>
<button onclick=go()>Opslaan</button>
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
<h2>Fallback-configuratie</h2>
<p id=s>...</p>
<input type=file id=f accept=".json,application/json">
<button onclick=up()>Upload</button>
<p><a href=/fallback/config download=fallback.json>Huidige configuratie downloaden</a></p>
<button onclick=clr()>Configuratie wissen</button>
<pre id=o></pre>
<script>
async function st(){let r=await fetch('/fallback/status');let j=await r.json();
s.textContent='Modus: '+(j.active?'FALLBACK':'normaal')+' | Home Assistant: '+(j.ha?'verbonden':'weg')+' | regels: '+j.rules}
async function up(){if(!f.files[0]){o.textContent='Kies eerst een bestand';return}
let t=await f.files[0].text();
let r=await fetch('/fallback',{method:'POST',headers:{'Content-Type':'text/plain'},body:t});
o.textContent=await r.text();st()}
async function clr(){if(!confirm('Fallback-configuratie wissen?'))return;
let r=await fetch('/fallback',{method:'DELETE'});o.textContent=await r.text();st()}
st()
</script>)HTML";

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

// ---------------------------------------------------------------- setup

void FallbackEngine::add_input(binary_sensor::BinarySensor *sensor) {
  uint8_t idx = this->inputs_.size();
  this->inputs_.push_back(sensor);
  // sensor staat op inverted: state==true betekent "actief" (knop ingedrukt / schakelaar aan)
  sensor->add_on_state_callback([this, idx](bool state) { this->on_input_(idx, state); });
}

void FallbackEngine::set_reset_input(binary_sensor::BinarySensor *sensor) {
  sensor->add_on_state_callback([this](bool state) {
    // De eerste callback is de initiele toestand na het opstarten: negeren.
    // Alleen een echte druk (stijgende flank) binnen het tijdvenster telt.
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

  this->server_ = std::make_unique<WebServer>(this->port_);
  this->register_routes_();
}

void FallbackEngine::dump_config() {
  ESP_LOGCONFIG(TAG, "Fallback engine:");
  ESP_LOGCONFIG(TAG, "  Inputs: %u, outputs: %u", (unsigned) this->inputs_.size(), (unsigned) this->outputs_.size());
  ESP_LOGCONFIG(TAG, "  Rules in NVS: %u", (unsigned) this->rules_.size());
  ESP_LOGCONFIG(TAG, "  Port: %u, password: %s", this->port_, this->has_password_() ? "set" : "NOT SET (setup pending)");
  ESP_LOGCONFIG(TAG, "  Disconnect delay: %ums, reconnect delay: %ums", (unsigned) this->disconnect_delay_,
                (unsigned) this->reconnect_delay_);
}

// ---------------------------------------------------------------- NVS: regels

void FallbackEngine::load_rules_() {
  this->rules_.clear();
  Preferences p;
  if (!p.begin("fallback", false)) {
    ESP_LOGW(TAG, "NVS namespace kon niet geopend worden");
    return;
  }
  uint16_t n = p.getUShort("count", 0);
  if (n == 0 || n > MAX_RULES) {
    p.end();
    return;
  }
  std::vector<Rule> tmp(n);
  size_t bytes = n * sizeof(Rule);
  size_t got = p.getBytes("rules", tmp.data(), bytes);
  uint32_t stored_hash = p.getUInt("hash", 0);
  p.end();

  if (got != bytes || fnv1a((const uint8_t *) tmp.data(), bytes) != stored_hash) {
    ESP_LOGE(TAG, "Fallback-config in NVS is beschadigd, genegeerd");
    return;
  }
  this->rules_ = std::move(tmp);
  ESP_LOGI(TAG, "%u fallback-regels geladen", (unsigned) this->rules_.size());
}

void FallbackEngine::save_rules_() {
  Preferences p;
  if (!p.begin("fallback", false)) return;
  if (this->rules_.empty()) {
    p.remove("rules");
    p.remove("hash");
    p.remove("count");
  } else {
    size_t bytes = this->rules_.size() * sizeof(Rule);
    p.putBytes("rules", this->rules_.data(), bytes);
    p.putUInt("hash", fnv1a((const uint8_t *) this->rules_.data(), bytes));
    p.putUShort("count", this->rules_.size());
  }
  p.end();
}

// ---------------------------------------------------------------- NVS: wachtwoord

void FallbackEngine::load_password_() {
  Preferences p;
  if (!p.begin("fallback", false)) return;
  String s = p.getString("pw", "");
  p.end();
  this->pass_ = s.c_str();
}

void FallbackEngine::reset_password() {
  Preferences p;
  if (p.begin("fallback", false)) {
    p.remove("pw");
    p.end();
  }
  this->pass_.clear();
  ESP_LOGW(TAG, "Wachtwoord gewist: stel een nieuw wachtwoord in via /fallback");
}

void FallbackEngine::handle_setup_() {
  if (this->has_password_()) {
    this->server_->send(403, "text/plain", "Wachtwoord is al ingesteld");
    return;
  }
  String body = this->server_->arg("plain");
  size_t n = body.length();
  if (n < PASS_MIN || n > PASS_MAX) {
    this->server_->send(400, "text/plain", "Het wachtwoord moet 8 tot 64 tekens lang zijn");
    return;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = body[i];
    if (c < 0x20 || c == 0x7f) {
      this->server_->send(400, "text/plain", "Ongeldige tekens in wachtwoord");
      return;
    }
  }
  Preferences p;
  if (!p.begin("fallback", false)) {
    this->server_->send(500, "text/plain", "Opslaan mislukt");
    return;
  }
  p.putString("pw", body);
  p.end();
  this->pass_ = body.c_str();
  ESP_LOGI(TAG, "Wachtwoord ingesteld");
  this->server_->send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------- JSON

bool FallbackEngine::parse_config_(const std::string &body, std::vector<Rule> &out, std::string &err) {
  err = "ongeldige JSON";
  bool ok = json::parse_json(body, [&](JsonObject root) -> bool {
    if ((int) (root["version"] | 0) != 1) {
      err = "version moet 1 zijn";
      return false;
    }
    JsonArray arr = root["rules"].as<JsonArray>();
    if (arr.isNull()) {
      err = "'rules' ontbreekt";
      return false;
    }
    if (arr.size() > MAX_RULES) {
      err = "te veel regels";
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
        snprintf(buf, sizeof buf, "regel %u: ongeldige ingang", (unsigned) line);
        err = buf;
        return false;
      }
      if (o < 0 || o >= (int) this->outputs_.size()) {
        snprintf(buf, sizeof buf, "regel %u: ongeldige uitgang", (unsigned) line);
        err = buf;
        return false;
      }
      if (ti < 0 || ai < 0) {
        snprintf(buf, sizeof buf, "regel %u: ongeldige trigger/actie", (unsigned) line);
        err = buf;
        return false;
      }
      if ((ai == ACT_ON_FOR || ai == ACT_OFF_FOR) && (sec < 1 || sec > 65535)) {
        snprintf(buf, sizeof buf, "regel %u: seconds 1-65535", (unsigned) line);
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

// ---------------------------------------------------------------- webserver

bool FallbackEngine::auth_() {
  if (!this->has_password_()) {
    this->server_->send(403, "text/plain", "Stel eerst een wachtwoord in via /fallback");
    return false;
  }
  if (this->server_->authenticate(USERNAME, this->pass_.c_str())) return true;
  this->server_->requestAuthentication();
  return false;
}

void FallbackEngine::register_routes_() {
  this->server_->on("/fallback", HTTP_GET, [this]() {
    if (!this->has_password_()) {
      this->server_->send(200, "text/html", SETUP_PAGE);  // eerste gebruik: wachtwoord kiezen
      return;
    }
    if (!this->auth_()) return;
    this->server_->send(200, "text/html", PAGE);
  });
  this->server_->on("/fallback/setup", HTTP_POST, [this]() { this->handle_setup_(); });
  this->server_->on("/fallback", HTTP_POST, [this]() {
    if (!this->auth_()) return;
    this->handle_upload_();
  });
  this->server_->on("/fallback", HTTP_DELETE, [this]() {
    if (!this->auth_()) return;
    this->rules_.clear();
    this->cancel_timers_();
    this->save_rules_();
    this->server_->send(200, "text/plain", "Configuratie gewist");
  });
  this->server_->on("/fallback/config", HTTP_GET, [this]() {
    if (!this->auth_()) return;
    this->server_->send(200, "application/json", this->rules_to_json_().c_str());
  });
  this->server_->on("/fallback/status", HTTP_GET, [this]() {
    if (!this->auth_()) return;
    bool ha = api::global_api_server != nullptr && api::global_api_server->is_connected();
    char buf[96];
    snprintf(buf, sizeof buf, "{\"active\":%s,\"ha\":%s,\"rules\":%u}", this->active_ ? "true" : "false",
             ha ? "true" : "false", (unsigned) this->rules_.size());
    this->server_->send(200, "application/json", buf);
  });
}

void FallbackEngine::handle_upload_() {
  String body = this->server_->arg("plain");
  if (body.length() == 0) {
    this->server_->send(400, "text/plain", "Lege upload");
    return;
  }
  std::vector<Rule> parsed;
  std::string err;
  if (!this->parse_config_(std::string(body.c_str()), parsed, err)) {
    std::string msg = "Afgekeurd: " + err;
    this->server_->send(400, "text/plain", msg.c_str());
    return;
  }
  this->rules_ = std::move(parsed);
  this->cancel_timers_();
  this->save_rules_();
  char buf[64];
  snprintf(buf, sizeof buf, "OK: %u regels opgeslagen", (unsigned) this->rules_.size());
  ESP_LOGI(TAG, "%s", buf);
  this->server_->send(200, "text/plain", buf);
}

// ---------------------------------------------------------------- runtime

void FallbackEngine::loop() {
  if (!this->server_started_ && network::is_connected()) {
    this->server_->begin();
    this->server_started_ = true;
    ESP_LOGI(TAG, "Upload-pagina actief op /fallback (poort %u)", this->port_);
  }
  if (this->server_started_) this->server_->handleClient();

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
    ESP_LOGW(TAG, "Home Assistant weg -> FALLBACK actief (%u regels)", (unsigned) this->rules_.size());
  } else if (this->active_ && ha_up && stable >= this->reconnect_delay_) {
    this->active_ = false;
    this->cancel_timers_();
    ESP_LOGW(TAG, "Home Assistant terug -> normale modus");
  }
}

void FallbackEngine::on_input_(uint8_t idx, bool state) {
  if (!this->active_) return;
  uint32_t now = millis();
  if (now - this->last_edge_[idx] < DEBOUNCE_MS) return;
  this->last_edge_[idx] = now;

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
  this->timer_active_[o] = false;  // elke nieuwe actie annuleert een lopende timer
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
  // via de light-entiteit, zodat de toestand correct naar HA gesynchroniseerd wordt bij reconnect
  auto call = this->outputs_[o]->make_call();
  call.set_state(on);
  call.set_transition_length(0);
  call.perform();
}

}  // namespace fallback_engine
}  // namespace esphome
