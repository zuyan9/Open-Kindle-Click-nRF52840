# Open Kindle Click (nRF52840)

Firmware that turns a Seeed Studio XIAO nRF52840 into a remote that a Kindle accepts as a Kindle Click: page forward and back, light up and down, and sleep. The Kindle also shows the remote's battery level.

Status: it passes an end-to-end test against Linux BlueZ but hasn't been tried on a Kindle yet. The same identity, report map and battery reporting work on a Kindle Scribe (3rd gen) and a Kindle Paperwhite (12th gen), firmware 5.20.1, in the [Android version](https://github.com/zuyan9/Open-Kindle-Click-Android).

Hardware
--------

A XIAO nRF52840 or XIAO nRF52840 Sense, and four push buttons, each between a pin and GND:

| Pin | Button | Key |
|---|---|---|
| D0 | top face | page forward |
| D1 | bottom face | page back |
| D2 | upper side | light up (repeats while held) |
| D3 | lower side | light down (repeats while held) |

- Hold both side buttons for 1 s to put the Kindle to sleep.
- Hold both face buttons for 10 s to forget the Kindle and go back to pairing mode.
- A LiPo cell on the BAT pads is optional. The Kindle shows its level; on USB the remote reports 100%. On battery, the remote stops advertising after 5 minutes without its Kindle; any button wakes it.

LED: fast blue blinks in pairing mode, a short blue blink every 2 s while waiting for its Kindle, off when connected, and a short green flash on each key sent (red when the battery is under 20%).

Build and flash
---------------

In the Arduino IDE, install the **Seeed nRF52 Boards** package (board manager URL `https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json`; not the "mbed-enabled" one). Choose Seeed XIAO nRF52840 Sense (or Seeed XIAO nRF52840), open `OpenKindleClick/OpenKindleClick.ino` and upload. The board package needs a `python` command that runs Python 3 (on Debian and Ubuntu: `sudo apt install python-is-python3`) and, on Linux, `adafruit-nrfutil` on the PATH (`pipx install adafruit-nrfutil`).

If the upload can't find the board, double-tap RESET, choose the port that appears and upload again.

Pairing
-------

On the Kindle, open Settings › Wi-Fi & Bluetooth › Bluetooth devices and choose the remote; there is no code to confirm. Settings › Accessories then shows its battery level and firmware version.

To pair it again, forget it on the Kindle (Bluetooth devices › the remote › Forget), then hold both face buttons for 10 s.

USB serial (115200 baud) takes test commands: `n`/`p` page forward/back, `+`/`-` light, `s` sleep, `i` status, `c` forget the Kindle; `h` lists them all.

Known limits
------------

- The remote can't wake the Kindle; no remote can. After you wake it, a page turn pressed while it reconnects (about 3 s) is delivered once it's ready.
- Kindle firmware 5.20.1 doesn't count remote page turns as activity, so the Kindle still sleeps when its sleep timer runs out after the last screen touch. A longer timer helps (Settings › Device options › Sleep timer).
- Only Kindle models with Kindle Click support accept it.

[PROTOCOL.md](PROTOCOL.md) describes what the Kindle checks and why the firmware does what it does.

Not affiliated with Amazon. Kindle is a trademark of Amazon.com, Inc. or its affiliates.
