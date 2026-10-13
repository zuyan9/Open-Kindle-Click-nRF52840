# Kindle Click protocol (from the Kindle side)

**Summary.** The Click is a plain BLE HID-over-GATT (HOGP) remote. The Kindle
recognizes it by Amazon VID `0x0171` / PID `0x0443` in two places:
1. the advertisement, during the pairing scan;
2. the DIS PnP ID, after connecting.

No Class of Device is involved, and nothing authenticates the remote
cryptographically. The remote must implement the Kindle's documented
**sleep gesture** itself (long-press both side buttons, which sends one
`KEY_POWER`).

Addresses below are in the 5.20.1 binaries:
- `libbluetooth.so` is Amazon's Bluedroid fork, in ARM mode.
- `acsbtfd` and `input_manager` are Thumb-2.

Items marked *(unverified)* come from a single static-analysis pass and still
need a live test.

## 1. Advertising (discovery / pairing scan)

| AD type | Content | Why |
|---|---|---|
| `0x01` Flags | `0x06`: LE General Discoverable + **BR/EDR not supported** | General inquiry only reports LE devices with a discoverable flag. Bit `0x04` (or a random address) also makes the Kindle classify the device as LE-only; see below |
| `0xFF` Manufacturer Specific | `71 01 04 43` = company `0x0171` (LE) + PID `0x0443` (**big-endian**) | The source of the VID/PID property the Kindle classifies on |
| `0x09` Name | e.g. `Open Kindle Click` (scan response is fine) | **Optional.** A remote that advertised no name at all (the Flipper Zero app) was listed and paired. Bluedroid replaces the name anyway (below) |

Placement rules:
- Flags and `71 01 04 43` must be in the **ADV_IND** itself, not only the scan response. Bluedroid's discovery does not hold an ADV_IND back for its SCAN_RSP.
- `71 01 04 43` must be the **first** `0xFF` AD; only the first is parsed.
- Never advertise UUID16 **`0xFE03`**. It sets BTA service bit 31, which skips the Amazon-remote branch.
- Avoid an 8-byte manufacturer payload that starts with ASCII `1949`.

LE-only classification:
- `btm_ble_update_inq_result` (`0x128818`–`0x128834`) marks a device dual-mode when Flags bit `0x04` is clear, the advert is undirected, and the address is public. Random addresses are never dual-mode.
- `acsbtfd` pairs with transport AUTO (`aceBT_pair(addr, 0)`). BTM uses LE only for device type BLE.
- So a dual-mode-classified LE remote would be bonded and opened over BR/EDR, and both would fail.

### What the Kindle's Bluedroid does with it

The scan-result handler is the `btif_dm` search callback, function at `0xac404`:

1. It calls `BTM_CheckEirData` for AD `0x09`/`0x08` (name), `0x10` (Device ID) and `0xFF` (manufacturer data).
2. **AD `0x10` Device ID** (length 8: `src16 vid16 pid16 ver16`) yields a VID/PID only for *non-Amazon* VIDs. VID `0x0171` or `0x1949` is routed to the manufacturer-data parser (`0xacd70`).
3. **AD `0xFF`** with length > 3: `company = LE16(data[0..1])`, `pid = BE16(data[2..3])`. If `btif_in_is_amazon_ble_remote(company, pid)` (`0xf89f8`) is true:
   - the name is replaced with `"Amazon Fire TV Remote"`, or `"Amazon Fire Game Controller"` for PID `0x0419` (`0xad6f8`);
   - a vendor property **type `0xFE` = `{u32 vid, u32 pid}`** is emitted (`0xac9e8`).
4. `btif_in_is_amazon_ble_remote` is true only for VID `0x0171` and PID in `{0x0411–0x0415, 0x0418, 0x0419, 0x041C, 0x041E, 0x0420, 0x0421, 0x0423, 0x0424, 0x0443}`. The Fire TV remotes and **`0x0443` (Kindle Click)** share this list.
5. Property `0xFE` is **not** part of the device-found property array. It goes out in its own one-property `remote_device_properties_cb` (`0xac97c`–`0xaca48`) just before `device_found_cb`. See §2 for how it is merged back.

Two other manufacturer-data layouts exist but don't apply to the Click:
- Exactly 8 ASCII hex chars `"VVVVPPPP"`. Only Lab126 VID `0x1949` is accepted; anything else falls through to the general parser.
- An Amazon "gadget" format with `LE16(data[4..5]) == 0x0171` and a 32-bit id. It is parsed when the Amazon-remote branch is skipped (UUID `0xFE03` present) or fails, and emits property `0x103`, which `acsbtfd` ignores.

### Scan parameters

A discovery run (LIPC `SetBtDiscover`, not looped) is a combined general
inquiry:
- the LE part is an active scan, stopped after **8 s**;
- duplicate filtering is off, but each LE device is reported at most once per run;
- there is no RSSI gate and no controller (APCF) filter to satisfy.

So the remote must be advertising during those 8 s. If it isn't listed, wake
it and scan again.

### Class of Device / Appearance

**No Class of Device is required.** An LE remote can't send one anyway:
- AD `0x0D` is never parsed;
- the peer's GAP Appearance (`0x2A01`) is never read.

Bluedroid makes up a CoD in `btm_ble_update_inq_result`, using the first rule that applies:

| Input | CoD |
|---|---|
| Appearance AD `0x19` = `0x03C1` (keyboard) | `0x000540` |
| Appearance AD `0x19` = `0x0180` (generic remote) | `0x00050C` |
| Appearance AD `0x19` = `0x03C0` | `0x000500` |
| Appearance AD `0x19`, any other value | `0x001F00` |
| No Appearance; UUID `0x1812` in AD `0x03` (complete list) | `0x000500` |
| Name starts with `Aspen`, or equals `Fire HD10 Keyboard` | `0x0005C0` |
| Otherwise | stored DevClass, or `0x001F00` |

The CoD's only effects over LE are:
- for keyboard CoDs, a no-op HID SET_IDLE;
- a metrics tag.

LE pairing callbacks hard-code CoD `0x1F00`, and `acsbtfd`'s only CoD rule is an
A2DP-sink fallback that LE devices can never match (§2). This firmware
advertises no Appearance, like the
[Android remote](https://github.com/zuyan9/Open-Kindle-Click-Android).

## 2. Classification in `acsbtfd`

How property `0xFE` reaches the classifier:
1. `libacehal_bt` (`separateProperties`) converts the HAL properties.
2. The ACE manager (`libace_bt_manager_core`) saves the last `0xFE`/`0x103` property in one slot keyed by address (`save_discovery_eir`, `0x398e0`). It appends it to the next device-found for the same address (`0x33576`–`0x3358c`) and keeps a per-discovery cache.
3. The `DEVICE_FOUND` IPC goes to `libace_bt`, which rebuilds `{0xFE, 8}`.
4. `BtAceAdapter::ExtractDevice` (`0x323d8`) stores `vid`/`pid` at struct offsets 60/62.

This relies on both Bluedroid callbacks firing in sequence on the btif thread.

`BtDiscoveryFilter::Classify` (`0x22574`) checks these tables, which are built in
static initializers (`0x1a128`):

| Table | Entries |
|---|---|
| VID/PID → type | `{0x0171, 0x0443} → 1` (`BleHidPageTurner`), used only if that type is enabled |
| UUID16 → type | `{0x110B} → 0` (A2DP AudioSink). **There is no `0x1812` HID entry.** |
| CoD fallback | audio service bit `0x200000` **and** an audio minor class (`0x404`, `0x408`, `0x414`, `0x418`, `0x41C`, `0x420`, `0x428`) → A2DP |
| PID → generation | `0x0443 → 1` (`bt.page_turner.gen`) |

- Type 1 is enabled by devcap feature **`kindleremote`**, or by the environment variable `BTFD_PAGE_TURNER_ENABLED=1`. `/var/local/system/bt_device_types.conf` is persistence (`type`, `base_name`, `pid`, `slot`), not a gate.
- A device that doesn't match is logged as `rejected` and not exposed. `acsbtfd` keeps no state for it, so it is re-evaluated on the next device-found, normally the next scan.
- The UI does not show the advertised name. It shows the devcap `kindleclick` name, numbered for extra remotes; Bluedroid's stored name ("Amazon Fire TV Remote") is used only if that is empty.

## 3. Pairing, GATT and battery

**Pairing**
- The Kindle starts bonding. LE Secure Connections Just Works (NoInputNoOutput) completes without a dialog. The host needs only **encryption without MITM** on HID attributes.
- A remote that declares a display with yes/no input gets numeric comparison instead: the Kindle shows the code with a Confirm button, and pairing completes once both sides confirm (Paperwhite 5.20.1 with the Flipper Zero app).
- **Security Requests from the remote are dropped** for any peer whose stored VID/PID is on the Amazon-remote list (except `0x0419`), bonded or not. The drop is in `smp_data_received` (`0x15d398`). Only the Kindle starts pairing or encryption, so a remote must never rely on its own Security Request.
- On an encryption failure (e.g. the remote lost its bond), the Kindle disconnects with reason 5 and removes the remote from background reconnect (`btm_sec_disconnected` → `BTM_BleUpdateBgConnDev(false)`), but **keeps the bond**. To re-pair: Forget the remote on the Kindle, reset the remote, then pair.
- After bonding, `acsbtfd` shows the `page_turner_paired_dialog`, and the UI's Connect does a GATT connect (30 s). Then:
  - `acsbtfd`'s own GATT client reads DIS Firmware Revision `0x2A26`, once per connection; the value is reset at disconnect. It shows "unknown" only for an empty value, and stays blank if the characteristic is missing or unreadable.
  - Then an explicit HID connect.
- *(unverified, design choice)* This firmware does **not** distribute its IRK. It always advertises with its static address. If a central stores an IRK for it and its controller can't switch that resolving-list entry to Device privacy mode, it would ignore the remote's adverts.

**GATT**

| Service | Characteristic | Requirement |
|---|---|---|
| HID `0x1812` | Report Map; each input Report with Report Reference (non-zero ID, type Input) and CCCD | **Essential**. Encrypted, no MITM. HID Info, Control Point, Protocol Mode and boot reports are optional for this host, which opens HID in Report mode (it never reads or writes Protocol Mode). This firmware sends one 10-byte report, ID 1: the boot-keyboard layout, then a 16-bit consumer usage. |
| Device Information `0x180A` | **PnP ID `0x2A50` = exactly `01 71 01 43 04 vv vv`** (BT SIG, VID `0x0171`, PID `0x0443`, version) | HOGP passes VID/PID/version to `uhid`; there is no Amazon override. The version is stored as `persist.sys.remote.sku`. |
| Device Information `0x180A` | Firmware Revision `0x2A26` | Shown as "Firmware version" in Settings > Accessories |
| Battery `0x180F` | Battery Level `0x2A19`, readable + **notify** | See below |

**Battery.** The Kindle discards its discovery-time read of `0x2A19`. Its
explicit `getBatteryLevel` query (run on every connect) works only if the Report
Map declares Battery Strength (usage page `0x06`, usage `0x20`) under a Report
ID. Otherwise:
- **only notifications** deliver the level to the UI and to the low (≤ 20 %) and critical (≤ 5 %) alerts;
- they are dropped until HID is open, so notify after the Kindle enables the CCCD and its writes settle;
- re-notify now and then even when the value hasn't changed.

**Other Amazon-remote special cases** (keyed on the stored VID/PID):
- The PnP version is stored in `persist.sys.remote.sku`.
- HID Information's bcdHID is not copied over the PnP version.
- Connection interval:
  - forced to 20 ms (CE 5–8.75 ms) on direct connects and updates (`l2cble_adjust_conn_params`, `0x197244`);
  - 7.5 ms during pairing and GATT discovery;
  - background links use 30–50 ms, latency 0 and a **20 s** supervision timeout.
- PPCP is stored but doesn't set the interval.
- An encryption failure removes the remote from background reconnect (above).
- Remote-name updates are ignored.
- A 2-byte SET_REPORT starting with `0xF2` is rerouted as DATA.

## 4. Buttons and keys

The Kindle's own UI documents the real Click. Sources:
- `pillow.mo` `pageTurnerPairedPopup*`
- `mainapp.mo` `settingsapp.remote_detail.*`
- Settings > Accessories > *remote*

`devices.json` matches evdev devices by `vid=0x0171 pid=0x0443` only
(`EVIOCGID`) → category `page_turner`. `input_manager` grabs the node
(`EVIOCGRAB`) and sets autorepeat 500/333 ms (`EVIOCSREP`).

| Click button (Kindle UI) | evdev key | Kindle action | HID usage used here |
|---|---|---|---|
| Button 1, top face | `KEY_PAGEDOWN` | page forward (default), re-emitted on `kindle-input-pt` | Keyboard `0x4E` |
| Button 2, bottom face | `KEY_PAGEUP` | page back (default), re-emitted on `kindle-input-pt` | Keyboard `0x4B` |
| Button 3, upper side | `KEY_BRIGHTNESSUP` | powerd `stepBrightnessWithUI(+1)` on press **and every autorepeat** | Consumer `0x006F` |
| Button 4, lower side | `KEY_BRIGHTNESSDOWN` | powerd `stepBrightnessWithUI(-1)` on press **and every autorepeat** | Consumer `0x0070` |
| "Long press both side buttons at the same time" | `KEY_POWER` | `system.sleep` = powerd `setPowerButtonIfActiveAsync(1)`, on press | Consumer `0x0030` |

**The remote must do the sleep gesture itself.** `input_manager` does no
long-press or chord detection for `page_turner`:
- `trigger_detection: false`;
- every mapping is press-only.

So the remote must:
1. time the long press;
2. stop any brightness key, so the Kindle stops autorepeating it;
3. send exactly one `KEY_POWER`.

`KEY_POWER` is honoured only while the Kindle is active. It can put the Kindle
to sleep but never wake it (§5).

Pipeline (in order):
1. Capability filter: drops other keys and `EV_MSC`.
2. Orientation swap: forward/back swap at 180°.
3. Debounce: a **press** of a key within 50 ms of the previous accepted press of the same key is dropped. Releases and repeats are never filtered or delayed.
4. Rate limiter: 100/s, burst 10, presses only.

Notes:
- **Holding a page button turns exactly one page.** X's evdev driver drops autorepeat (value 2), and Xorg runs with `-ardelay 0`. A lost page-key release only swallows the next press of that key.
- **Lost releases still matter for brightness.** A lost brightness release ramps the light until the next consumer report.
- Brightness autorepeat needs `EV_REP` on the evdev node. Linux `hid-input` sets it only for keyboard-page usages, and `hid-generic` creates one node per application collection. So the consumer usages must share an application collection with the keyboard usages.
- Kernel 6.6.77-lab126 has `uhid` and `hid-generic` built in. No Amazon HID driver claims `0x0171`.
- "Configure page forward / backwards" in Settings > Accessories:
  - swaps `KEY_PAGEDOWN`/`KEY_PAGEUP` in category scope (`/var/local/input_manager/mappings.ini`);
  - so `KEY_PAGEDOWN` must physically be the **top** face button, or the Kindle's labels are reversed.

User path (5.20.1):
- **Pair:** Settings > Wi-Fi and Bluetooth > Bluetooth devices.
- **Details:** Settings > Accessories > *remote*. It shows:
  - battery, with "Remote light will switch to red if battery is under 20%";
  - firmware version;
  - page direction;
  - the button guide.
- **Forget:** Bluetooth devices > *remote* > Forget.

## 5. Reconnection and sleep

**Who reconnects.** Only Bluedroid's HID host reconnects the remote, using a
white-list background connection (`bta_hh_le_add_device`, `0x7a8c4`).
`acsbtfd`'s `BtReconnect` and the ACE reconnection manager never reconnect page
turners. The background scan is set to **30 ms every 1.28 s**, so reconnect
latency is set by the remote's advertising interval. Measured on an awake
Paperwhite (the remote dropped the link, 5 reconnects each), it is longer than
that scan alone would give:

| Advertising interval | Median | Max |
|---|---|---|
| 20 ms | 2.5 s | 4.9 s |
| 30 ms | 6.2 s | 11.3 s |
| 50 ms | 3.9 s | 13.6 s |
| 100 ms | 12.5 s | 22.1 s |
| 152.5 ms | 7.7 s | 21.8 s |

So the remote should advertise at 20 ms the whole time it waits for its Kindle.

When the background connection is armed and disarmed:

| Armed by | Disarmed by |
|---|---|
| HID-host enable, for bonded devices with a stored `HidAttrMask` (written at the first HID open) | a UI Disconnect |
| each successful HID open with a known LE key | unpair |
| a GATT close with reason `0x08` | a GATT service-search failure (`0x7bfc4`) |
| a Bluetooth toggle, reboot, or suspend/resume (re-arms) | a HID discovery-complete error (`0x7b91c`) |
| a UI Connect (makes a 30 s attempt) | a Report Reference or Battery Level read failing with `0x05` |
| | a key-missing encryption failure (§3) |

**Every connection** runs a full GATT rediscovery (the cache is freed at
disconnect), then HID open, then `uhid` re-creation, then the `input_manager`
grab. Reports sent earlier are silently lost. Observable anchors for "ready":
- the Kindle's notification CCCD writes (cold connects);
- the interval leaving 7.5 ms (end of discovery);
- the interval moving to 20 ms with no latency (the only one when the Kindle stayed awake).

Measured on a Paperwhite 12th gen (5.20.1):
- After a wake (Android remote) it encrypted at about +0.6 s, left 7.5 ms at +1.7–2.2 s and wrote its CCCDs at +1.8–2.2 s, in either order. A key 0.02 s after the first CCCD write was lost; one 0.5 s after it worked.
- After a wake with this firmware advertising at 20 ms, it reconnected about 4 s after the power button and was ready 2.2 s later.
- After the remote dropped the link while the Kindle stayed awake, it connected at 48.75 ms, moved to 26.25 ms at +0.6 s, encrypted at +0.8 s and moved to 20 ms with no latency at +1.6–2.4 s. There was no 7.5 ms phase and usually no CCCD write. Keys at that move were lost; keys 0.3 s and more after it worked.

The Kindle connects from a resolvable private address.

**Sleep.**
- At powerd suspend level 6, about 15 s before kernel suspend, `acsbtfd` powers Bluetooth fully off (`ForceAllOff` → `aceBT_disableRadio`, `wmt_cli bluetooth off`). A Paperwhite kept the link for about 100 s after going to sleep, then disconnected (reason `0x13`).
- Every remote, including the real Click, disconnects and **cannot wake the Kindle**.
- On resume the persisted Bluetooth intent is restored and the bonded remote is re-armed.
- So the first press after a wake needs a reconnect. The remote should queue that page turn and deliver it once the Kindle is ready.
- Remote keys don't count as activity: the sleep timer (Settings > Device options > Sleep timer) still runs from the last touch. Neither `input_manager` nor `libAccessories.so` calls powerd's `setTouchSsTimeoutAsync`.

**GATT layout changes.**
- For one Bluetooth session the Kindle keeps the HID handles (8-bit), the protocol-mode handle and the Report Map. Service Changed makes it rediscover, but it ignores the remote's reports until the next connection.
- After flashing a build that changes services, characteristics, the report count or the Report Map: Forget and re-pair. This always works.

## 6. Kindle-side prerequisites

- devcap feature `kindleremote` must be `1`. It is set per model, not by firmware version or release year.
- Firmware with `acsbtfd` page-turner support (5.20.x here).

## Known from the Kindle UI (no Click needed)

- **Layout:**
  - two face buttons: top = page forward, bottom = page back (default);
  - two buttons on one side edge: upper = brightness up, lower = brightness down.
- **Sleep:** "long press both side buttons at the same time". The remote times this and sends `KEY_POWER`.
- **LED:** "Remote light will switch to red if battery is under 20%".

## Unknowns for the hardware

- How long the Click's "long press" is. This firmware uses 1 s.
- The Click's pairing gesture and how it clears its own bonds.
- Extra bytes after `71 01 04 43` in the real advertisement (none are needed).
- The real Report Map. Any map works if keyboard and consumer usages share one application collection.
- Other LED colours and patterns, and the real advertising and sleep timing.

Kindle runtime details still to measure:
- The MT6631 privacy-mode support.
