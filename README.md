<p align="center">
  <a href="https://www.logi.casa">
    <img src="assets/logo.png" alt="Logi.casa" width="320">
  </a>
</p>

<h1 align="center">ESP Home Controller</h1>

<p align="center">
  Open ESPHome firmware for the <a href="https://www.logi.casa/shop/esp-home-controller/">Logi.casa ESP Home Controller</a>:<br>
  48 inputs, 32 outputs, Ethernet or WiFi, native Home Assistant integration, and a built-in <b>offline fallback</b>.
</p>

---

## Contents

- [What is it?](#what-is-it)
- [Hardware at a glance](#hardware-at-a-glance)
- [Install the firmware](#install-the-firmware)
- [Quick start](#quick-start)
- [Offline fallback](#offline-fallback)
  - [How it works](#how-it-works)
  - [Set up the fallback page](#set-up-the-fallback-page)
  - [The fallback file](#the-fallback-file)
  - [Example](#example)
  - [Behaviour details](#behaviour-details)
- [Status LED](#status-led)
- [Controls in Home Assistant](#controls-in-home-assistant)
- [Updating the firmware](#updating-the-firmware)
- [Security](#security)
- [Troubleshooting](#troubleshooting)
- [Repository layout](#repository-layout)
- [License](#license)
- [Support](#support)

---

## What is it?

The **ESP Home Controller** is a DIN-rail controller that puts 48 digital inputs and 32 digital outputs into
[Home Assistant](https://www.home-assistant.io/) over Ethernet (or WiFi, with the WiFi firmware). It runs
[ESPHome](https://esphome.io/) on an ESP32, so it is easy to adopt, update over the network, and reflash with your own firmware.

This repository contains the firmware configuration and the **fallback engine**. The fallback engine keeps your
lights working when Home Assistant is down: if the controller loses its connection to Home Assistant, it switches
to a set of simple rules that you defined in advance (for example *"wall button 3 toggles the kitchen light"*).

> **Why does this matter?** Home Assistant runs on a server, and servers get updated, restarted and sometimes fail.
> With the fallback, a restart or an outage never leaves anyone standing in the dark.

## Hardware at a glance

| | |
|---|---|
| **Inputs** | 48 digital inputs, 24 VDC (active on a positive signal) |
| **Outputs** | 32 digital outputs, 24 VDC, max. 500 mW per channel (switch to ground) |
| **Power** | 24 VDC, 15 W max. (below 1 W with no active channels) |
| **Network** | Ethernet (RJ45), or WiFi with the WiFi firmware variant |
| **Processor** | ESP32 |
| **In Home Assistant** | 48 binary sensors, 32 lights, a status LED light, IP and MAC address sensors |

Product page: <https://www.logi.casa/shop/esp-home-controller/>
Hardware installation (wiring, power, network): <https://www.logi.casa/esp-home-controller-quick-start/>

> ⚠️ Outputs are limited to 500 mW. Use them to drive relays, contactors or LED feedback, not loads directly.
> Work in a power cabinet should be done by a qualified person.

---

## Install the firmware

Controllers are delivered with the firmware already installed. To install or reinstall it, use the installer page:

**<https://syntos.github.io/esp-controller-test/>**

1. Choose your variant: **Ethernet** (network cable) or **WiFi**.
2. Open the casing, connect the mainboard to your computer with a data-capable micro-USB cable, and put the **DIP switch in FLASH mode**.
3. Click **Install firmware** and choose the serial port. Use Chrome or Edge on a computer.
4. When it has finished, set the DIP switch back. With the WiFi variant, the page then asks for your WiFi name and password.

The same page contains the [fallback editor](#the-fallback-file).

---

## Quick start

You need a running Home Assistant installation with the **ESPHome Device Builder** add-on (ESPHome 2026.9 or newer).
Wiring the controller (power, inputs, outputs, network cable) is described in the
[hardware quick start](https://www.logi.casa/esp-home-controller-quick-start/).

1. **Connect and power the controller.** Plug in the network cable (Ethernet variant) and apply 24 VDC. The status LED lights up red while starting.
2. **Open the ESPHome Device Builder** in Home Assistant. The controller appears as a discovered device. Click **Take Control**.
3. **Give it a unique name** (add a number if you have several controllers) and let ESPHome install the latest firmware.
   ESPHome creates a **unique encryption key** for your controller during this step. You never have to type or copy it.
4. When the upload has finished, the status LED turns **blue**: the controller is connected to Home Assistant.
5. In Home Assistant, go to **Settings → Devices & services**, find the discovered ESPHome device, click **Add**, then **Submit**.
   You do not need to assign a room. Entities are created automatically.

You now have these entities:

| Entity | Type | Meaning |
|---|---|---|
| `INPUT_01` … `INPUT_48` | binary sensor | The 48 inputs |
| `CHANNEL_01` … `CHANNEL_32` | light | The 32 outputs |
| `Status LED` | light | The LED on the controller |
| `IP Address`, `MAC Address` | text sensor | Network information |
| Reboot, fallback reset buttons | button | See [Controls in Home Assistant](#controls-in-home-assistant) |

**Try it:** create an automation with a *State* trigger on `INPUT_01` (to `on`) and a *Light → Toggle* action on `CHANNEL_01`.

---

## Offline fallback

### How it works

| Situation | What happens |
|---|---|
| Home Assistant is connected | Normal operation. Home Assistant controls everything. The fallback rules do nothing. |
| Home Assistant is gone for **15 seconds** | The controller switches to **fallback mode** and starts applying your saved rules. |
| Home Assistant is back for **10 seconds** | The controller returns to **normal mode**. Home Assistant takes over again. |

This also applies after a power cut or restart while Home Assistant is not yet available.
The rules are stored in the controller's own non-volatile memory, so they survive restarts.

### Set up the fallback page

1. Find the controller's IP address (the `IP Address` sensor in Home Assistant, or your router).
2. Open `http://<controller-ip>/fallback` in a browser **on your local network**.
3. The first time, the page asks you to **choose a password** (8 to 64 characters). Afterwards, log in with user name **`admin`** and that password.
4. Choose your fallback file and click **Upload**. The controller checks the file and answers with the number of rules it saved, or with the reason it was rejected.
5. Use **Download current configuration** to retrieve what is stored, and **Clear configuration** to remove all rules.

> The page and your password travel over plain HTTP inside your local network. Do not reuse a password from elsewhere.

### The fallback file

A fallback file is a small JSON file. Every **rule** connects one **input** to one **output**:

```json
{
  "version": 1,
  "rules": [
    { "in": 0, "trigger": "both",   "out": 0, "action": "toggle" },
    { "in": 7, "trigger": "rising", "out": 5, "action": "on_for", "seconds": 120 }
  ]
}
```

**Numbering starts at 0.** `INPUT_01` is `"in": 0` and `INPUT_48` is `"in": 47`. `CHANNEL_01` is `"out": 0` and `CHANNEL_32` is `"out": 31`.

| Field | Values | Meaning |
|---|---|---|
| `in` | `0` – `47` | The input that triggers the rule |
| `out` | `0` – `31` | The output that is controlled |
| `trigger` | `both` | Every change of the input: use for **wall switches** |
| | `rising` | Only when the input becomes active (**button pressed**) |
| | `falling` | Only when the input becomes inactive (**button released**) |
| `action` | `toggle` | Flip the output |
| | `on` / `off` | Switch the output on / off |
| | `on_for` | Switch on, then off after `seconds` |
| | `off_for` | Switch off, then on again after `seconds` |
| `seconds` | `1` – `65535` | Only for `on_for` and `off_for` |

You can have **up to 256 rules**. One input may have several rules, so a single button can control several outputs.

#### Names and rule groups (optional)

The visual editor lets you give inputs, outputs and rules a name, and lets one rule use several inputs and several outputs. The file then contains two extra top-level sections next to `rules`:

```json
{
  "version": 1,
  "rules": [ ... ],
  "groups": [
    { "name": "Kitchen light", "in": [1, 7], "trigger": "rising", "out": [1], "action": "toggle" }
  ],
  "labels": { "in": { "1": "Kitchen switch" }, "out": { "1": "Kitchen" } }
}
```

The controller only ever acts on `rules` (every input and output pair of a group becomes one entry there, so the 256 limit counts these entries). `groups` and `labels` are only used by the editor. The controller stores them next to the rules (up to 16 KB) and returns them with **Download current configuration**, so you can edit your setup from any computer.

> **Easiest way:** build the file visually with the [Fallback editor](https://syntos.github.io/esp-controller-test/#editor). Pick inputs and outputs, test the rules in the built-in simulator, and download the file. You can also open an existing file to change it.

### Example

[`fallback-test.json`](fallback-test.json) covers every trigger and action:

| Input | Trigger | Output | Result |
|---|---|---|---|
| `INPUT_01` | both | `CHANNEL_01` | Toggles with every change of a wall switch |
| `INPUT_02` | rising | `CHANNEL_02` | Push button toggles the light |
| `INPUT_03` | rising + falling | `CHANNEL_03` | On while the button is held |
| `INPUT_04` | rising | `CHANNEL_04` | On for 10 seconds, then off |
| `INPUT_05` | rising | `CHANNEL_05` | Off for 10 seconds, then on again |
| `INPUT_06` | rising | `CHANNEL_01` – `04` | One button switches four lights off |
| `INPUT_07` | falling | `CHANNEL_07` | Toggles when the button is released |

Adapt the numbers to the inputs and outputs you actually wired.

### Behaviour details

- **Fallback rules only run in fallback mode.** In normal mode they never interfere with your Home Assistant automations.
- **A new action on an output cancels a running timer** on that output. Triggering `on_for` again restarts the countdown.
- **Timers are cancelled** when the controller returns to normal mode. Outputs keep their current state, and Home Assistant picks them up from there.
- **Input changes are debounced** (30 ms), so a bouncing contact does not trigger a rule twice.
- **An invalid file is rejected as a whole.** The previously stored rules stay active.

---

## Status LED

| Colour | Meaning |
|---|---|
| 🔴 Red | Starting up, or no connection to Home Assistant since power-up |
| 🔵 Blue | Connected to Home Assistant |
| 🟣 Purple | Connection to Home Assistant lost (less than 15 seconds ago) |
| 🟠 Orange | Home Assistant has been gone for 15 seconds: **fallback mode is active** |

Only Home Assistant counts for the LED and for the fallback. Other connections to the controller, such as the log viewer in ESPHome Builder, do not change the colour.

---

## Controls in Home Assistant

The controller adds a few configuration buttons to its device page in Home Assistant:

| Button | What it does |
|---|---|
| **Reboot** | Restarts the controller |
| **Reset fallback password** | Removes the fallback page password. You choose a new one the next time you open `/fallback`. Stored rules are kept. |
| **Clear fallback rules** | Removes all stored fallback rules. The password is kept. |

### Resetting the password without Home Assistant

If Home Assistant is down and you forgot the password, the fallback page can be reset with a push button, when enabled with
`reset_input` in the configuration (input 48 in the default setup).

1. Restart the controller. Within **60 seconds** of starting, press the button and **hold it for 10 seconds**. The status LED blinks white while you hold.
2. When the LED turns **steady green**, the password has been reset: release the button. The LED returns to its normal colour.
3. Open `http://<controller-ip>/fallback` and choose a new password.

Your stored rules are kept. Do not hold the button while the controller is starting (it is then ignored until you release and press again), and use a **push button**, not a switch.
If you use this input as the reset button, do not use it in fallback rules (`"in": 47`).

---

## Updating the firmware

Updates are installed from the **ESPHome Device Builder** in Home Assistant: open your controller and click **Install**.
Updates travel over the network and are protected by your controller's encryption key. Your configuration follows the
release it was created with, so a new release only reaches you when you choose to update.

If a controller ever becomes unreachable, reflash it over **micro-USB** with the [installer page](#install-the-firmware).
Your fallback rules and password are kept, because the installer does not erase the controller's settings memory.

---

## Security

- **Home Assistant link:** after you take control, the connection is encrypted with a unique key per controller. Before that, a controller uses a public factory key that is the same for every unit, so take control right after installing.
- **Fallback page:** it is protected by a password, but runs over plain HTTP. Use it **only on your local network**. Never forward its port to the internet.
- **Physical access matters:** anyone with access to the reset button or the USB port can reset or reflash the controller.
- **No shared secrets:** this repository contains no keys or passwords, and none should ever be committed to it.

---

## Troubleshooting

| Problem | What to check |
|---|---|
| The LED stays **red** | The controller has no network. Check the network cable and your router. With the WiFi variant, enter your WiFi details again via the installer page. |
| The LED is **purple** or **orange** | The controller cannot reach Home Assistant. Check that Home Assistant is running and on the same network. |
| Home Assistant asks for an **encryption key** | Open the controller in the ESPHome Device Builder, choose *Show API key*, and paste it in. |
| `/fallback` does not open | Use `http://`, not `https://`. Check the IP address and that you are on the same network. |
| `/fallback` shows the password setup again | No password is stored (new device or after a reset). Choose a new one. |
| The upload is rejected | An input is not between 0 and 47, an output is not between 0 and 31, `seconds` is missing for `on_for` / `off_for`, or the JSON is invalid. |
| Fallback rules do nothing | Fallback mode only starts **15 seconds after** Home Assistant is gone. Check the mode shown on `/fallback`. Check that the numbers in your rules start at 0. |
| An output seems to ignore a rule | Another rule on the same output may cancel it, such as an `off` rule on the same trigger. |

---

## Repository layout

```
.
├── esp-controller-ethernet.yaml   Ready-to-use config, Ethernet variant (generated)
├── esp-controller-wifi.yaml       Ready-to-use config, WiFi variant (generated)
├── src/
│   ├── common.yaml                Shared hardware, entities and fallback setup
│   ├── network-ethernet.yaml      Ethernet specifics
│   └── network-wifi.yaml          WiFi specifics
├── components/
│   └── fallback_engine/           The fallback engine (ESPHome external component)
├── site/                          The website: installer and fallback editor
├── tools/                         Generators and checks
├── compat/                        Home Assistant entity names that must never change
├── example/customer-config.yaml   Example of a personal configuration
├── project.json                   Version, repository, ESPHome version
├── fallback-example.json          Small fallback example
├── fallback-test.json             Fallback example that covers every trigger and action
└── .github/workflows/             Build and publish
```

The two `esp-controller-*.yaml` files are **generated** from `src/`. They contain **no private secrets**. When you take control of a
controller in ESPHome, it generates a small configuration of your own that points to one of them and adds your own encryption key.

---

## Development

```
python tools/build_yaml.py        # regenerate esp-controller-*.yaml after editing src/
python tools/check_entities.py    # make sure no Home Assistant entity was renamed or removed
python tools/build_site.py        # regenerate site/index.html after editing site/index.template.html
```

Home Assistant builds entity IDs from the entity names, so renaming an input or output breaks users' automations.
CI refuses such a change unless you deliberately accept it with `python tools/check_entities.py --update`.
See [RELEASING.md](RELEASING.md) for how to publish a new version.

---

## License

The code in this repository is released under the [MIT License](LICENSE). The Logi.casa name and logo are trademarks of Logi.casa and are not covered by that license.

---

## Support

Questions or problems? Contact us through the
[Logi.casa support page](https://logi.casa/ticket/) or visit <https://www.logi.casa>.

<p align="center">
  <br>
  <a href="https://www.logi.casa"><img src="assets/logo.png" alt="Logi.casa" width="140"></a>
</p>
