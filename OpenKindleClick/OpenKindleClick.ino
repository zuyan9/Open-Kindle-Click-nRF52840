// Open Kindle Click: Seeed XIAO nRF52840 firmware that a Kindle accepts as a
// Kindle Click page turner. See ../PROTOCOL.md for where each requirement
// comes from.
//
// Buttons (to GND, internal pull-ups), laid out like the Kindle Click that the
// Kindle's own UI describes (pairing dialog; Settings > Accessories > remote):
//   D0 = Button 1, top face      -> page forward   (KEY_PAGEDOWN)
//   D1 = Button 2, bottom face   -> page backward  (KEY_PAGEUP)
//   D2 = Button 3, upper side    -> brightness up  (held: repeats)
//   D3 = Button 4, lower side    -> brightness down (held: repeats)
//   D2 + D3 held 1 s             -> Kindle sleeps (KEY_POWER), the Kindle's
//                                   documented "long press both side buttons"
//   D0 + D1 held 10 s            -> forget bonds, pairing mode. Also Forget the
//                                   remote on the Kindle (Bluetooth devices).
//
// USB serial (115200) accepts test commands; send "h" for the list.

#include <stdarg.h>

#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <nrfx_power.h>

#include "bluefruit_tweaks.h"

extern "C" void tusb_hal_nrf_power_event(uint32_t event);  // TinyUSB's USB power event handler

#define FW_VERSION   "0.3.1"
#define DEVICE_NAME  "Open Kindle Click"

// ---- Identity the Kindle checks -------------------------------------------
// Advertising: manufacturer data = company 0x0171 (little-endian), then the
// PID big-endian. libbluetooth.so only turns this into a VID/PID for PIDs on
// its Amazon-remote list, which includes 0x0443. It must sit in the ADV_IND
// (not the scan response) and be the only 0xFF AD.
static const uint8_t ADV_MFG_DATA[] = { 0x71, 0x01, 0x04, 0x43 };

// DIS PnP ID: source 0x01 (Bluetooth SIG), VID 0x0171, PID 0x0443, version
// 0x0100. HOGP hands VID/PID to uhid; input_manager matches on them.
static const uint8_t PNP_ID[7] = { 0x01, 0x71, 0x01, 0x43, 0x04, 0x00, 0x01 };

// ---- Pins ------------------------------------------------------------------
enum ButtonId { BTN_NEXT, BTN_PREV, BTN_BRI_UP, BTN_BRI_DOWN, BTN_COUNT };
static const uint8_t BUTTON_PINS[BTN_COUNT] = { D0, D1, D2, D3 };

// XIAO RGB LED is active-low (the variant's LED_STATE_ON is wrong for it).
#define LED_ON  LOW
#define LED_OFF HIGH

// ---- Timing ----------------------------------------------------------------
static const uint32_t DEBOUNCE_MS        = 15;
static const uint32_t SIDE_CHORD_MS      = 150;    // 2nd side button within this: no brightness
static const uint32_t SLEEP_HOLD_MS      = 1000;   // both side buttons -> KEY_POWER
static const uint32_t PAIR_HOLD_MS       = 10000;  // both face buttons -> forget bonds
// The Kindle drops a press that comes within 50 ms of the previous press of
// the same key; releases are never filtered. This only keeps press and
// release in separate reports.
static const uint32_t MIN_PRESS_MS       = 30;
// A woken Kindle reconnects within about a second and is ready about 3 s
// later, so a page turn still queued after this was pressed while it slept.
static const uint32_t PENDING_TTL_MS     = 10000;
// The Kindle drops reports until it has rediscovered GATT, opened HID and
// recreated uhid, which input_manager then grabs. Ready = quiet after the
// later of its last notification-enabling CCCD write (HID report, battery;
// not the Service Changed indication) and its last link change (leaving its
// 7.5 ms discovery interval, or moving to 20 ms with no latency), else a
// fallback. A Paperwhite (5.20.1) waking up wrote its CCCDs 1.8-2.2 s after
// connecting and took a key 0.5 s after the first write, but not 0.02 s after.
// Reconnecting while it stayed awake, it wrote no CCCDs and its only sign was
// the move to 20 ms; it took keys 0.3 s after that, but not at it.
static const uint32_t READY_QUIET_MS     = 800;
static const uint32_t READY_FALLBACK_MS  = 5000;
static const uint32_t CONN_PARAM_DELAY_MS = 1500;  // after ready
// Waiting for its Kindle, the remote stays at the fast interval: an awake
// Paperwhite reconnected after a median 2.5 s at 20 ms, but 12.5 s at 100 ms
// and 7.7 s at 152.5 ms. Only pairing mode slows down, after 30 s.
static const uint16_t ADV_FAST_INTERVAL  = 32;     // 20 ms
static const uint16_t ADV_SLOW_INTERVAL  = 244;    // 152.5 ms
static const uint32_t BATTERY_PERIOD_MS  = 60000;
static const uint32_t BATTERY_RENOTIFY_MS = 15UL * 60 * 1000;
static const uint8_t  BATTERY_LOW_PCT    = 20;     // Kindle UI: "red if battery is under 20%"
static const uint32_t DORMANT_AFTER_MS   = 5UL * 60 * 1000;  // bonded, on battery

// ---- HID -------------------------------------------------------------------
// One 10-byte input report (ID 1): the boot-keyboard layout, then one consumer
// usage, the layout the Android remote uses on the Scribe 3 and Paperwhite.
// One application collection: Linux hid-generic then makes one evdev node,
// and it has EV_REP (from the keyboard usages), so held brightness repeats.
static const uint8_t REPORT_MAP[] = {
  0x05, 0x01,                   // Usage Page (Generic Desktop)
  0x09, 0x06,                   // Usage (Keyboard)
  0xA1, 0x01,                   // Collection (Application)
  0x85, 0x01,                   //   Report ID (1)
  0x05, 0x07,                   //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0, 0x29, 0xE7,       //   Usage Min/Max (modifiers)
  0x15, 0x00, 0x25, 0x01,       //   Logical Min/Max (0..1)
  0x75, 0x01, 0x95, 0x08,       //   Report Size 1, Count 8
  0x81, 0x02,                   //   Input (Data, Var, Abs)
  0x75, 0x08, 0x95, 0x01,       //   Report Size 8, Count 1
  0x81, 0x01,                   //   Input (Const): reserved byte
  0x19, 0x00, 0x29, 0x65,       //   Usage Min/Max (0..0x65)
  0x15, 0x00, 0x25, 0x65,       //   Logical Min/Max (0..0x65)
  0x75, 0x08, 0x95, 0x06,       //   Report Size 8, Count 6
  0x81, 0x00,                   //   Input (Data, Array, Abs)
  0x05, 0x0C,                   //   Usage Page (Consumer)
  0x19, 0x00, 0x2A, 0xFF, 0x03, //   Usage Min/Max (0..0x3FF)
  0x15, 0x00, 0x26, 0xFF, 0x03, //   Logical Min/Max (0..0x3FF)
  0x75, 0x10, 0x95, 0x01,       //   Report Size 16, Count 1
  0x81, 0x00,                   //   Input (Data, Array, Abs)
  0xC0                          // End Collection
};

enum KeyId { KEY_NEXT, KEY_PREV, KEY_BRI_UP, KEY_BRI_DOWN, KEY_SLEEP, KEY_COUNT, KEY_NONE = 0xFF };

struct KeyDef {
  const char* name;
  bool        consumer;   // false = keyboard page
  uint16_t    usage;
};

// Linux hid-input maps these to the evdev codes in input_manager's table.
static const KeyDef KEYS[KEY_COUNT] = {
  { "page forward (KEY_PAGEDOWN)",          false, 0x4E },
  { "page backward (KEY_PAGEUP)",           false, 0x4B },
  { "brightness up (KEY_BRIGHTNESSUP)",     true,  0x006F },
  { "brightness down (KEY_BRIGHTNESSDOWN)", true,  0x0070 },
  { "sleep (KEY_POWER)",                    true,  0x0030 },
};

// ---- Button pairs ----------------------------------------------------------
// The face pair (D0/D1) and the side pair (D2/D3) share one state machine.
// A lone button sends its key (side buttons only after SIDE_CHORD_MS, so the
// sleep gesture never touches the light). Once both buttons of a pair are
// down, the pair's key is released and nothing else is sent until both are up
// again, except the pair's hold action.
enum ChordState { CH_IDLE, CH_WAIT, CH_SINGLE, CH_CHORD, CH_DONE };

struct ChordPair {
  ButtonId   a, b;
  KeyId      key_a, key_b;
  uint32_t   window;    // chord window before a lone press counts
  bool       rollover;  // releasing one of a chord restarts the other as a press
  ChordState st;
  ButtonId   first;
  uint32_t   since;     // CH_WAIT: first press; CH_CHORD: both down
};

struct Button {
  bool     raw;
  bool     pressed;
  uint32_t raw_since;
};

BLEDis        bledis;
BLEBas        blebas;
BLEHidGeneric blehid(1);

// ---- State -----------------------------------------------------------------
static volatile uint16_t g_conn = BLE_CONN_HANDLE_INVALID;
static volatile bool     g_secured = false;
static volatile uint32_t g_connected_at = 0;
static volatile uint32_t g_secured_at = 0;

// Readiness evidence, written in the BLE task (raw event callback).
static volatile uint32_t g_cccd_at = 0;        // last notification-enabling CCCD write
static volatile uint8_t  g_cccd_writes = 0;
static volatile uint32_t g_disc_done_at = 0;   // last link change: left 7.5 ms, or 20 ms with no latency
static volatile bool     g_fast_interval = false;
static bool              g_ready = false;      // latched per connection in loop()
static uint32_t          g_ready_at = 0;
static bool              g_params_requested = false;

static bool     g_dormant = false;
static uint32_t g_last_activity = 0;
static uint8_t  g_battery = 100;
static uint16_t g_battery_mv = 0;
static bool     g_battery_sent = false;        // notified on this connection
static uint32_t g_battery_sent_cccd = 0;       // g_cccd_at when we notified
static uint32_t g_battery_sent_at = 0;

static bool     g_down[KEY_COUNT];             // key state the host should see
static uint32_t g_down_at[KEY_COUNT];
static bool     g_release_wanted[KEY_COUNT];
static bool     g_dirty = false;               // report not yet accepted by the stack

static KeyId    g_pending = KEY_NONE;          // page turn queued while disconnected
static uint32_t g_pending_at = 0;

static bool     g_usb_replayed = false;        // USB "detected" replayed at boot
static bool     g_bonds_cached = false;
static uint32_t g_forget_until = 0;            // re-clear bond files rewritten late
static uint32_t g_flash_at = 0;                // last key feedback flash

static Button    g_btn[BTN_COUNT];
static ChordPair g_face = { BTN_NEXT,   BTN_PREV,     KEY_NEXT,   KEY_PREV,     0,             true,  CH_IDLE, BTN_NEXT,   0 };
static ChordPair g_side = { BTN_BRI_UP, BTN_BRI_DOWN, KEY_BRI_UP, KEY_BRI_DOWN, SIDE_CHORD_MS, false, CH_IDLE, BTN_BRI_UP, 0 };

// ---- Small helpers ---------------------------------------------------------
// Logging never blocks: BLE callbacks call this too, and a terminal that holds
// DTR but stops reading must not stall them.
static void logmsg(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n <= 0 || !Serial) return;
  if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
  if (Serial.availableForWrite() >= n) Serial.write((const uint8_t*)buf, n);
}

static bool usbPowered() {
  uint8_t sd = 0;
  sd_softdevice_is_enabled(&sd);
  uint32_t reg = 0;
  if (sd) sd_power_usbregstatus_get(&reg);
  else    reg = NRF_POWER->USBREGSTATUS;
  return reg & POWER_USBREGSTATUS_VBUSDETECT_Msk;
}

static bool connected() { return g_conn != BLE_CONN_HANDLE_INVALID; }

static void setLed(bool r, bool g, bool b) {
  digitalWrite(LED_RED,   r ? LED_ON : LED_OFF);
  digitalWrite(LED_GREEN, g ? LED_ON : LED_OFF);
  digitalWrite(LED_BLUE,  b ? LED_ON : LED_OFF);
}

static bool hasBonds() {
  // Bluefruit keeps one file per bonded central in this directory.
  using namespace Adafruit_LittleFS_Namespace;
  File dir("/adafruit/bond_prph", FILE_O_READ, InternalFS);
  if (!dir) return false;
  File f = dir.openNextFile(FILE_O_READ);
  bool any = (bool)f;
  if (f) f.close();
  dir.close();
  return any;
}

// ---- Link readiness --------------------------------------------------------
// Runs synchronously in the BLE task for every SoftDevice event.
static void onBleEvent(ble_evt_t* evt) {
  switch (evt->header.evt_id) {
    case BLE_GAP_EVT_CONNECTED:
      g_connected_at = millis();
      g_cccd_at = 0;
      g_cccd_writes = 0;
      g_disc_done_at = 0;
      g_fast_interval = (evt->evt.gap_evt.params.connected.conn_params.max_conn_interval == 6);
      break;
    case BLE_GAP_EVT_CONN_PARAM_UPDATE: {
      // 7.5 ms: the Kindle's GATT discovery. 20 ms with no latency: the link
      // the Kindle settles on once it has set the remote up.
      ble_gap_conn_params_t const& p = evt->evt.gap_evt.params.conn_param_update.conn_params;
      bool fast = (p.max_conn_interval == 6);
      if ((g_fast_interval && !fast) || (p.max_conn_interval == 16 && p.slave_latency == 0))
        g_disc_done_at = millis() | 1;
      g_fast_interval = fast;
      break;
    }
    case BLE_GATTS_EVT_WRITE: {
      ble_gatts_evt_write_t const& w = evt->evt.gatts_evt.params.write;
      if (w.uuid.uuid == BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG && w.len >= 1 &&
          (w.data[0] & BLE_GATT_HVX_NOTIFICATION)) {
        g_cccd_at = millis() | 1;
        g_cccd_writes++;
      }
      break;
    }
    default:
      break;
  }
}

static bool linkReady() {
  if (!connected() || !g_secured) return false;
  if (g_ready) return true;
  uint32_t now = millis();
  uint32_t c = g_cccd_at, d = g_disc_done_at;
  uint32_t last = !c ? d : !d ? c : (int32_t)(c - d) > 0 ? c : d;
  // Signed: a stamp from the BLE task can be 1 ms ahead of now.
  bool quiet = last && !g_fast_interval && (int32_t)(now - last) >= (int32_t)READY_QUIET_MS;
  if (quiet || now - g_secured_at >= READY_FALLBACK_MS) {
    g_ready = true;
    g_ready_at = now;
    uint32_t t0 = g_connected_at;
    char disc[24] = "no link change", cccd[32] = "no CCCD writes";
    if (d) snprintf(disc, sizeof disc, "link change +%lu", d - t0);
    if (c) snprintf(cccd, sizeof cccd, "%u CCCD writes, last +%lu", g_cccd_writes, c - t0);
    logmsg("[link] +%lu ms ready for keys (%s): encrypted +%lu, %s, %s\n",
           now - t0, quiet ? "host settled" : "fallback", g_secured_at - t0, disc, cccd);
  }
  return g_ready;
}

// ---- HID reports -----------------------------------------------------------
// The report carries the full key state, so resending the latest one is
// always correct; a refused report is retried on every loop() pass.
static bool sendReport() {
  uint8_t report[10] = { 0 };     // modifiers, reserved, 6 keys, consumer usage
  uint8_t n = 2;
  uint16_t usage = 0;
  for (int k = 0; k < KEY_COUNT; k++) {
    if (!g_down[k]) continue;
    if (KEYS[k].consumer) usage = KEYS[k].usage;  // last one wins
    else if (n < 8)       report[n++] = (uint8_t)KEYS[k].usage;
  }
  report[8] = usage & 0xFF;
  report[9] = usage >> 8;
  return blehid.inputReport(g_conn, 1, report, sizeof report);
}

static void flushReports() {
  if (!connected()) { g_dirty = false; return; }
  if (g_dirty && sendReport()) g_dirty = false;
}

static void startAdvertising();

static void ensureFastAdvertising() {
  if (connected()) return;
  if (Bluefruit.Advertising.isRunning() && Bluefruit.Advertising.getInterval() != ADV_FAST_INTERVAL) {
    Bluefruit.Advertising.stop();   // in the slow phase: restart the 30 s fast phase
  }
  startAdvertising();
}

static void keyDown(KeyId k) {
  g_last_activity = millis();
  if (!linkReady()) {
    ensureFastAdvertising();
    // Like a real remote: reconnect, then deliver the page turn. Only page
    // turns are worth replaying, and only to a Kindle we are bonded with.
    if ((k == KEY_NEXT || k == KEY_PREV) && g_bonds_cached) {
      g_pending = k;
      g_pending_at = millis();
      logmsg("[key] %s queued until the Kindle is ready\n", KEYS[k].name);
    } else {
      logmsg("[key] %s ignored: not connected\n", KEYS[k].name);
    }
    return;
  }
  if (g_down[k]) {                 // pressed again before its release went out
    g_release_wanted[k] = false;   // keep holding
    return;
  }
  g_down[k] = true;
  g_down_at[k] = millis();
  g_release_wanted[k] = false;
  g_dirty = true;
  flushReports();
  g_flash_at = millis();
  logmsg("[key] %s down\n", KEYS[k].name);
}

static void keyUp(KeyId k) {
  if (g_down[k]) g_release_wanted[k] = true;  // sent by serviceReleases()
}

static void serviceReleases() {
  for (int k = 0; k < KEY_COUNT; k++) {
    if (g_down[k] && g_release_wanted[k] && millis() - g_down_at[k] >= MIN_PRESS_MS) {
      g_down[k] = false;
      g_release_wanted[k] = false;
      g_dirty = true;
      logmsg("[key] %s up\n", KEYS[k].name);
    }
  }
  flushReports();
}

static void tap(KeyId k) {
  keyDown(k);
  keyUp(k);
}

// Release every key on the host, e.g. before we drop the link ourselves.
static void releaseAll() {
  bool any = false;
  for (int k = 0; k < KEY_COUNT; k++) {
    if (g_down[k]) any = true;
    g_down[k] = false;
    g_release_wanted[k] = false;
  }
  if (any) {
    g_dirty = true;
    flushReports();
  }
}

static void servicePending() {
  if (g_pending == KEY_NONE) return;
  if (millis() - g_pending_at > PENDING_TTL_MS) {
    logmsg("[key] dropped queued %s (the Kindle didn't reconnect: asleep?)\n", KEYS[g_pending].name);
    g_pending = KEY_NONE;
    return;
  }
  if (linkReady()) {
    KeyId k = g_pending;
    g_pending = KEY_NONE;
    tap(k);
  }
}

// ---- Battery ---------------------------------------------------------------
static uint8_t measureBattery() {
  // VBAT -> 1M / 510k divider -> P0.31; VBAT_ENABLE (P0.14) stays LOW.
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(PIN_VBAT);
  float mv = (sum / 16.0f) * 3600.0f / 4096.0f * (1510.0f / 510.0f);
  g_battery_mv = (uint16_t)mv;
  // On USB the pin shows the charger, not the cell (or no cell at all).
  if (usbPowered()) return 100;

  static const uint16_t MV[]  = { 3300, 3500, 3600, 3700, 3750, 3800, 3900, 4000, 4100, 4200 };
  static const uint8_t  PCT[] = {    0,    5,   10,   20,   30,   40,   60,   80,   90,  100 };
  if (mv <= MV[0]) return 0;
  for (int i = 1; i < 10; i++) {
    if (mv <= MV[i]) return PCT[i - 1] + (uint8_t)((mv - MV[i - 1]) * (PCT[i] - PCT[i - 1]) / (MV[i] - MV[i - 1]));
  }
  return 100;
}

static void notifyBattery(const char* why) {
  blebas.write(g_battery);
  if (!connected() || !blebas.notify(g_conn, g_battery)) return;  // not subscribed yet
  g_battery_sent = true;
  g_battery_sent_cccd = g_cccd_at;
  g_battery_sent_at = millis();
  logmsg("[bat] %u%% notified (%s)\n", g_battery, why);
}

static void serviceBattery() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (last == 0 || now - last >= BATTERY_PERIOD_MS) {
    last = now;
    uint8_t pct = measureBattery();
    bool changed = pct != g_battery;
    g_battery = pct;
    blebas.write(pct);
    if (changed && linkReady()) notifyBattery("changed");
  }
  if (!linkReady()) return;
  // The Kindle only learns the level from notifications, and only once it
  // has subscribed and opened HID: notify after its CCCD writes settle.
  uint32_t c = g_cccd_at;
  const char* why = NULL;
  if (!g_battery_sent)                                          why = "connected";
  else if (c != g_battery_sent_cccd && (int32_t)(now - c) >= 1000) why = "subscribed";
  else if (now - g_battery_sent_at >= BATTERY_RENOTIFY_MS)      why = "periodic";
  if (why) notifyBattery(why);
}

// ---- BLE -------------------------------------------------------------------
static void startAdvertising() {
  g_dormant = false;
  g_last_activity = millis();
  if (Bluefruit.Advertising.isRunning()) return;
  Bluefruit.Advertising.setFastTimeout(g_bonds_cached ? 0 : 30);  // 0: never slow down
  if (!Bluefruit.Advertising.start(0)) {
    logmsg("[ble] advertising failed to start\n");
    return;
  }
  logmsg(g_bonds_cached ? "[ble] advertising (waiting for bonded Kindle)\n"
                      : "[ble] advertising in pairing mode\n");
}

static void enterDormant() {
  Bluefruit.Advertising.stop();
  g_dormant = true;
  setLed(false, false, false);
  logmsg("[pwr] dormant: advertising stopped, any button wakes\n");
}

// Drop the link ourselves (all keys released first) and wait until it is gone.
static void dropLink() {
  if (!connected()) return;
  releaseAll();
  delay(100);                       // let the key-up notifications go out
  Bluefruit.disconnect(g_conn);
  uint32_t t0 = millis();
  while (connected() && millis() - t0 < 3000) delay(10);
}

static void forgetBonds() {
  logmsg("[ble] forgetting all bonds (also Forget the remote on the Kindle)\n");
  g_pending = KEY_NONE;
  // Keep the still-bonded Kindle from reconnecting between disconnect and
  // clearBonds().
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.stop();
  dropLink();
  Bluefruit.Periph.clearBonds();
  // Bluefruit saves CCCDs from a deferred callback after a disconnect, which
  // recreates a (keyless) bond file if it runs after clearBonds().
  g_forget_until = millis() + 3000;
  g_bonds_cached = false;
  Bluefruit.Advertising.restartOnDisconnect(true);
  startAdvertising();
}

static void onConnect(uint16_t conn) {
  g_conn = conn;
  g_secured = false;
  g_ready = false;
  g_params_requested = false;
  g_battery_sent = false;
  g_last_activity = millis();
  BLEConnection* c = Bluefruit.Connection(conn);
  if (!c) return;  // already gone
  ble_gap_addr_t a = c->getPeerAddr();
  logmsg("[ble] connected to %02X:%02X:%02X:%02X:%02X:%02X\n",
       a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1], a.addr[0]);
}

static void onSecured(uint16_t conn) {
  g_secured_at = millis();
  g_secured = true;
  BLEConnection* c = Bluefruit.Connection(conn);
  logmsg("[ble] link encrypted (bonded=%d)\n", c ? (int)c->bonded() : -1);
}

static void onPairComplete(uint16_t conn, uint8_t status) {
  // Bonded: Bluefruit's own restart after the next disconnect stays fast too.
  if (status == BLE_GAP_SEC_STATUS_SUCCESS) Bluefruit.Advertising.setFastTimeout(0);
  logmsg("[ble] pairing %s (status 0x%02X)\n",
       status == BLE_GAP_SEC_STATUS_SUCCESS ? "succeeded" : "FAILED", status);
}

static void onDisconnect(uint16_t conn, uint8_t reason) {
  g_conn = BLE_CONN_HANDLE_INVALID;
  g_secured = false;
  g_ready = false;
  for (int k = 0; k < KEY_COUNT; k++) {
    g_down[k] = false;
    g_release_wanted[k] = false;
  }
  g_dirty = false;
  g_last_activity = millis();
  logmsg("[ble] disconnected (reason 0x%02X)\n", reason);
}

static void setupBle() {
  Bluefruit.autoConnLed(false);
  // Queue several notifications: with the default of one, a key-up waits on
  // an unacknowledged key-down and can be refused.
  Bluefruit.configPrphConn(BLE_GATT_ATT_MTU_DEFAULT, BLE_GAP_EVENT_LENGTH_DEFAULT, 4,
                           BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT);
  Bluefruit.begin(1, 0);
  // On a cold start VBUS can become valid while the SoftDevice starts, and its
  // "USB detected" event is then lost (Bluefruit only catches up on "ready",
  // which never comes before USB is enabled). Replay it, as a plug-in would.
  if (usbPowered() && !NRF_USBD->ENABLE) {
    vTaskSuspendAll();
    tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_DETECTED);
    xTaskResumeAll();
    g_usb_replayed = true;
  }
  Bluefruit.setTxPower(4);
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.setEventCallback(onBleEvent);

  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  Bluefruit.Security.setIOCaps(false, false, false);  // Just Works
  Bluefruit.Security.setSecuredCallback(onSecured);
  Bluefruit.Security.setPairCompleteCallback(onPairComplete);
  bluefruitDontDistributeIdentityKey();

  // HID first, so later DIS/BAS changes never move the HID handles the Kindle
  // keeps for a session. Any GATT layout change still needs a re-pair. No boot
  // keyboard (nor Protocol Mode): the Kindle uses Report mode and never asks.
  static uint16_t input_len[] = { 10 };
  blehid.setHidInfo(0x0111, 0x00, 0x03);  // RemoteWake | NormallyConnectable
  blehid.setReportLen(input_len);
  blehid.setReportMap(REPORT_MAP, sizeof REPORT_MAP);
  blehid.begin();

  blebas.begin();
  blebas.write(g_battery);

  bledis.setManufacturer("Open Kindle Click project");
  bledis.setModel("XIAO nRF52840");
  bledis.setFirmwareRev("nRF52840 " FW_VERSION);  // the Kindle's Settings > Accessories > Firmware version
  bledis.setPNPID((const char*)PNP_ID, sizeof PNP_ID);
  bledis.begin();

  // ADV_IND: Flags + manufacturer data first (both required there), then the
  // HID UUID. The name goes in the scan response.
  bool ok = Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  ok &= Bluefruit.Advertising.addManufacturerData(ADV_MFG_DATA, sizeof ADV_MFG_DATA);
  ok &= Bluefruit.Advertising.addService(blehid);
  if (!ok) logmsg("[ble] ADV payload full: Kindle identification may be missing\n");
  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(ADV_FAST_INTERVAL, ADV_SLOW_INTERVAL);
}

static void serviceConnection() {
  if (!linkReady() || g_params_requested) return;
  if (millis() - g_ready_at < CONN_PARAM_DELAY_MS) return;
  g_params_requested = true;
  // 20 ms (what the Kindle uses), latency 4, 3 s supervision timeout instead
  // of the Kindle's 20 s, so a silently lost link is noticed quickly.
  BLEConnection* c = Bluefruit.Connection(g_conn);
  if (c) c->requestConnectionParameter(16, 4, 300);
}

// ---- Buttons ---------------------------------------------------------------
static ChordPair& pairOf(ButtonId b) { return (b == BTN_NEXT || b == BTN_PREV) ? g_face : g_side; }
static KeyId soloKey(const ChordPair& p, ButtonId b) { return b == p.a ? p.key_a : p.key_b; }

static void sleepAction() {
  // Never queued: it could put the Kindle to sleep right after it wakes.
  if (!linkReady()) { logmsg("[key] sleep ignored: not connected\n"); return; }
  tap(KEY_SLEEP);
}

static void forgetAction() {
  releaseAll();
  forgetBonds();
}

static void startSingle(ChordPair& p, ButtonId b) {
  p.st = CH_SINGLE;
  p.first = b;
  keyDown(soloKey(p, b));
}

static void onPress(ButtonId b) {
  ChordPair& p = pairOf(b);
  switch (p.st) {
    case CH_IDLE:
      if (p.window == 0) startSingle(p, b);
      else { p.st = CH_WAIT; p.first = b; p.since = millis(); }
      break;
    case CH_SINGLE:
      keyUp(soloKey(p, p.first));  // stop it (and the Kindle's autorepeat)
      // fall through
    case CH_WAIT:
    case CH_CHORD:                 // a re-press restarts the hold timer
      p.st = CH_CHORD;
      p.since = millis();
      break;
    case CH_DONE:                  // wait for both released
      break;
  }
}

static void onRelease(ButtonId b) {
  ChordPair& p = pairOf(b);
  switch (p.st) {
    case CH_WAIT:
      if (b == p.first) { tap(soloKey(p, b)); p.st = CH_IDLE; }
      break;
    case CH_SINGLE:
      if (b == p.first) { keyUp(soloKey(p, b)); p.st = CH_IDLE; }
      break;
    case CH_CHORD:
    case CH_DONE:
      if (!g_btn[p.a].pressed && !g_btn[p.b].pressed) {
        p.st = CH_IDLE;
      } else if (p.st == CH_CHORD && p.rollover) {
        ButtonId still = (b == p.a) ? p.b : p.a;   // rolled onto the other button
        if (p.window == 0) startSingle(p, still);
        else { p.st = CH_WAIT; p.first = still; p.since = millis(); }
      }
      break;
    case CH_IDLE:
      break;
  }
}

static void pollPair(ChordPair& p, uint32_t hold_ms, void (*action)()) {
  uint32_t now = millis();
  if (p.st == CH_WAIT && now - p.since >= p.window) {
    startSingle(p, p.first);
  } else if (p.st == CH_CHORD && g_btn[p.a].pressed && g_btn[p.b].pressed &&
             now - p.since >= hold_ms) {
    p.st = CH_DONE;
    action();
  }
}

static void pollButtons() {
  uint32_t now = millis();
  for (int i = 0; i < BTN_COUNT; i++) {
    bool raw = digitalRead(BUTTON_PINS[i]) == LOW;
    if (raw != g_btn[i].raw) {
      g_btn[i].raw = raw;
      g_btn[i].raw_since = now;
    } else if (raw != g_btn[i].pressed && now - g_btn[i].raw_since >= DEBOUNCE_MS) {
      g_btn[i].pressed = raw;
      if (raw) onPress((ButtonId)i);
      else     onRelease((ButtonId)i);
    }
  }
  pollPair(g_side, SLEEP_HOLD_MS, sleepAction);   // documented: long press both side buttons
  pollPair(g_face, PAIR_HOLD_MS, forgetAction);   // not a Kindle gesture: safe for reset
}

// ---- LED -------------------------------------------------------------------
static void updateLed() {
  uint32_t now = millis();
  bool low = g_battery < BATTERY_LOW_PCT;
  if (now - g_flash_at < 40) { setLed(low, !low, false); return; }  // key feedback, red when low
  if (g_dormant)           { setLed(false, false, false); return; }

  if (connected()) {
    setLed(low && now % 5000 < 30, false, false);
  } else if (!g_bonds_cached) {
    setLed(false, false, now % 500 < 100);     // pairing mode: fast blue
  } else {
    setLed(low && now % 2000 < 30, false, now % 2000 < 30);  // reconnecting: blue blip
  }
}

// ---- Serial console --------------------------------------------------------
static void printStatus() {
  logmsg("---- Open Kindle Click " FW_VERSION " ----\n");
  ble_gap_addr_t a = Bluefruit.getAddr();
  logmsg("address     %02X:%02X:%02X:%02X:%02X:%02X\n",
       a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1], a.addr[0]);
  logmsg("identity    VID 0x0171 / PID 0x0443 (mfg data 71 01 04 43)\n");
  logmsg("connected   %s%s%s\n", connected() ? "yes" : "no",
       g_secured ? ", encrypted" : "", g_ready ? ", ready" : "");
  BLEConnection* c = connected() ? Bluefruit.Connection(g_conn) : NULL;
  if (c) logmsg("link        interval %u x1.25 ms, latency %u, timeout %u0 ms\n",
                c->getConnectionInterval(), c->getSlaveLatency(), c->getSupervisionTimeout());
  logmsg("advertising %s%s\n", Bluefruit.Advertising.isRunning() ? "yes" : "no",
       g_dormant ? " (dormant)" : "");
  logmsg("bonds       %s\n", hasBonds() ? "yes" : "none (pairing mode)");
  logmsg("battery     %u%% (%u mV)  usb=%s\n", g_battery, g_battery_mv, usbPowered() ? "yes" : "no");
  logmsg("reset       0x%lX (0 = power-on)%s\n", readResetReason(),
       g_usb_replayed ? ", USB detect replayed" : "");
}

static void printHelp() {
  logmsg("commands: n=page forward  p=page backward  +=brightness up  -=brightness down\n");
  logmsg("          s=sleep  i=status  c=forget bonds/pair  a=advertise  z=dormant  x=disconnect\n");
  logmsg("          d=reboot to UF2 bootloader  h=help\n");
}

static void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'n': case 'f': case ']': tap(KEY_NEXT); break;
      case 'p': case 'b': case '[': tap(KEY_PREV); break;
      case '+': case '=':           tap(KEY_BRI_UP); break;
      case '-': case '_':           tap(KEY_BRI_DOWN); break;
      case 's':                     sleepAction(); break;
      case 'i':                     printStatus(); break;
      case 'c':                     forgetBonds(); break;
      case 'a':                     ensureFastAdvertising(); break;
      case 'z':                     enterDormant(); break;
      case 'x':                     dropLink(); break;   // test reconnection
      case 'd':
        logmsg("rebooting into UF2 bootloader...\n");
        dropLink();                 // don't leave the Kindle waiting out a timeout
        Serial.flush();
        delay(100);
        enterUf2Dfu();
        break;
      case 'h': case '?':           printHelp(); break;
      default: break;
    }
  }
}

// ---- Arduino ---------------------------------------------------------------
void setup() {
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  setLed(false, false, false);

  for (int i = 0; i < BTN_COUNT; i++) pinMode(BUTTON_PINS[i], INPUT_PULLUP);

  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);  // keep P0.31 below its limit while charging
  analogReference(AR_DEFAULT);     // 3.6 V full scale
  analogReadResolution(12);
  analogSampleTime(40);            // divider source is ~338 kOhm

  Serial.begin(115200);

  setupBle();
  g_battery = measureBattery();
  blebas.write(g_battery);
  g_bonds_cached = hasBonds();
  startAdvertising();

  logmsg("Open Kindle Click " FW_VERSION " ready. Send 'h' for commands.\n");
}

void loop() {
  pollButtons();
  servicePending();
  serviceReleases();
  handleSerial();
  serviceBattery();
  serviceConnection();

  static uint32_t last_bond_check = 0;
  if (g_forget_until && !connected() && millis() - last_bond_check >= 100) {
    last_bond_check = millis();
    if ((int32_t)(millis() - g_forget_until) >= 0) {
      g_forget_until = 0;
    } else if (hasBonds()) {
      Bluefruit.Periph.clearBonds();
      logmsg("[ble] removed a bond file rewritten after the reset\n");
    }
  } else if (!g_dormant && millis() - last_bond_check > 1000) {
    last_bond_check = millis();
    g_bonds_cached = hasBonds();
  }
  updateLed();

  if (!g_dormant && !connected() && g_bonds_cached && !usbPowered() &&
      millis() - g_last_activity > DORMANT_AFTER_MS) {
    enterDormant();
  }

  delay(g_dormant ? 50 : 5);
}
