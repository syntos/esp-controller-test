<p align="center">
  <a href="https://www.logi.casa">
    <img src="assets/logo.png" alt="Logi.casa" width="320">
  </a>
</p>

<h1 align="center">ESP Home Controller</h1>

<p align="center">
  Open ESPHome firmware for the <a href="https://www.logi.casa/shop/esp-home-controller/">Logi.casa ESP Home Controller</a>:<br>
  48 inputs, 32 outputs, wired Ethernet, native Home Assistant integration, and a built-in <b>offline fallback</b>.
</p>

---

## Contents

- [What is it?](#what-is-it)
- [Hardware at a glance](#hardware-at-a-glance)
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
[Home Assistant](https://www.home-assistant.io/) over a wired Ethernet connection. It runs
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
| **Network** | Wired Ethernet (RJ45) |
| **Processor** | ESP32 |
| **In Home Assistant** | 48 binary sensors, 32 lights, a status LED light, IP and MAC address sensors |

Product page: <https://www.logi.casa/shop/esp-home-controller/>
Hardware installation (wiring, power, network): <https://www.logi.casa/esp-home-controller-quick-start/>

> ⚠️ Outputs are limited to 500 mW. Use them to drive relays, contactors or LED feedback, not loads directly.
> Work in a power cabinet should be done by a qualified person.

---

## Quick start

You need a running Home Assistant installation with the **ESPHome Device Builder** add-on (ESPHome 2026.9 or newer).
Wiring the controller (power, inputs, outputs, network cable) is described in the
[hardware quick start](https://www.logi.casa/esp-home-controller-quick-start/).

1. **Connect and power the controller.** Plug in the network cable and apply 24 VDC. The status LED lights up red while starting.
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

> **Easiest way:** build the file visually with the [Fallback editor](https://syntos.github.io/esp-controller-test/). Pick inputs and outputs, test the rules in the built-in simulator, and download the file. You can also open an existing file to change it.

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
| 🔴 Red | Starting up |
| 🔵 Blue | Connected to Home Assistant |
| 🟣 Purple | Connection to Home Assistant lost |
| 🟠 Orange | Home Assistant has been gone for 15 seconds: **fallback mode is active** |

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
Updates travel over the network (Ethernet) and are protected by your controller's encryption key.

If a controller ever becomes unreachable, you can reflash it over **micro-USB**: put the DIP switch in flash mode,
connect the cable, flash with [ESPHome Web](https://web.esphome.io/) (Chrome or Edge), set the switch back, and restart.

---

## Security

- **Home Assistant link:** the connection between the controller and Home Assistant is encrypted with a unique key per controller, created when you take control of it.
- **Fallback page:** it is protected by a password, but runs over plain HTTP. Use it **only on your local network**. Never forward its port to the internet.
- **Physical access matters:** anyone with access to the reset button or the USB port can reset or reflash the controller.
- **No shared secrets:** this repository contains no keys or passwords, and none should ever be committed to it.

---

## Troubleshooting

| Problem | What to check |
|---|---|
| The LED stays **red** | The controller has no network. Check the network cable and your router. |
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
├── esp-controller.yaml            ESPHome package: hardware, entities, fallback setup
├── components/
│   └── fallback_engine/           The fallback engine (ESPHome external component)
├── example/
│   └── customer-config.yaml       Example configuration with your own secrets
├── docs/
│   └── index.html                 Fallback editor (served by GitHub Pages)
├── fallback-example.json          Small fallback example
├── fallback-test.json             Fallback example that covers every trigger and action
├── secrets.yaml.example           Template for your own secrets file
├── LICENSE                        MIT license
└── assets/
    └── logo.png
```

`esp-controller.yaml` is the shared package. It deliberately contains **no secrets**. When you take control of a controller in ESPHome, it
generates a small configuration of your own that points to this package and adds your own encryption key.

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
