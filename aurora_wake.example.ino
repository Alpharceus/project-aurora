/*
 * Aurora — WT32-ETH01 wake device + presence sensor node (v2)
 * WiFi (STA) carries the trigger; magic packet exits the Ethernet jack
 * point-to-point into the PC's wired NIC.
 *
 * v2 additions: BLE phone presence via IRK resolution (NimBLE).
 *   - /pair?token=…   opens a 2-min pairing window; pair once from the
 *                     phone's Bluetooth settings. The phone's IRK is
 *                     captured from the bond and stored in NVS.
 *   - passive scans then resolve the phone's randomized (RPA) addresses
 *     forever after; no app needed on the phone.
 *   - arrival after >=10 min away fires the magic packet automatically.
 *   - /forget?token=… wipes bonds + IRK for re-pairing.
 * Motion (LD2410 mmWave) lands in a later OTA once the sensor is here.
 *
 * Board: WT32-ETH01 (arduino-esp32 core; FQBN
 *   esp32:esp32:wt32-eth01:PartitionScheme=min_spiffs — v2 needs the
 *   1.9MB app slots; partition scheme was set at the Aug 15 serial flash.)
 */

// ---------- fill these in ----------
const char* WIFI_SSID   = "YOUR_SSID";        // 2.4 GHz network (ESP32 has no 5/6 GHz radio)
const char* WIFI_PASS   = "YOUR_PASS";
const char* WAKE_TOKEN  = "pick-a-long-random-string";
// PC Realtek NIC MAC, from `getmac /v` (format: AA:BB:CC:DD:EE:FF)
const uint8_t PC_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
const char* FW_VERSION  = "2.1";
// -----------------------------------

// Presence tuning — runtime-adjustable via /config, persisted in NVS
uint32_t cfgAwayS       = 600;   // unseen this many seconds -> away
bool     cfgArrivalWake = true;  // fire magic packet on arrival
int      cfgRssiMin     = -100;  // ignore sightings weaker than this (dBm)
const uint32_t PAIR_WINDOW_MS = 2UL * 60UL * 1000UL;   // /pair advertising window

// WT32-ETH01 PHY wiring (must be defined before ETH.h)
#define ETH_PHY_TYPE  ETH_PHY_LAN8720
#define ETH_PHY_ADDR  1
#define ETH_PHY_MDC   23
#define ETH_PHY_MDIO  18
#define ETH_PHY_POWER 16
#define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN

#include <ETH.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include "nimble/nimble/host/include/host/ble_store.h"

// Point-to-point subnet on the wire. Nothing else lives here; the
// broadcast address pins the packet's route to the ETH interface.
IPAddress ETH_IP(192, 168, 50, 1);
IPAddress ETH_GW(192, 168, 50, 1);
IPAddress ETH_MASK(255, 255, 255, 0);
IPAddress ETH_BCAST(192, 168, 50, 255);

WebServer server(80);
WiFiUDP udp;
Preferences prefs;

// ---- presence state (written from NimBLE host task, read in loop task) ----
uint8_t  phoneIRK[16];
bool     haveIRK      = false;
uint8_t  idAddr[6];              // phone's identity address (post-resolution)
bool     haveIdAddr   = false;
volatile bool     phonePresent = false;
volatile uint32_t lastSeenMs   = 0;
volatile bool     everSeen     = false;
volatile int      lastPhoneRssi = 0;
volatile uint32_t cntAdv = 0, cntRpa = 0, cntMatch = 0, cntArrivalWakes = 0;
volatile uint32_t cntIdMatch = 0;
volatile uint32_t cntByType[4] = {0, 0, 0, 0};  // public/random/public_id/random_id
bool     pairingOpen  = false;
uint32_t pairWindowEnd = 0;

// Debug ring of recently heard RPAs (little-endian, as received)
uint8_t  rpaRing[8][6];
volatile uint8_t rpaRingPos = 0;

void sendMagicPacket() {
  uint8_t pkt[102];
  memset(pkt, 0xFF, 6);
  for (int i = 1; i <= 16; i++) memcpy(pkt + i * 6, PC_MAC, 6);
  // Send a burst; WoL is fire-and-forget and repeats are free.
  for (int i = 0; i < 3; i++) {
    udp.beginPacket(ETH_BCAST, 9);
    udp.write(pkt, sizeof(pkt));
    udp.endPacket();
    delay(50);
  }
}

// ---- IRK / RPA resolution ------------------------------------------------
// BT spec: RPA(48b) = prand(24 MSB) | hash(24 LSB); hash = ah(IRK, prand),
// ah = AES128(k, 0^104 | prand) mod 2^24. Addresses arrive little-endian:
// addr[0..2] = hash, addr[3..5] = prand, (addr[5] & 0xC0) == 0x40 marks RPA.
// IRK byte order out of the bond store varies by stack; check both.
static bool ahMatches(const uint8_t* key /*big-endian for mbedtls*/,
                      const uint8_t* addr) {
  uint8_t in[16] = {0};
  in[13] = addr[5]; in[14] = addr[4]; in[15] = addr[3];  // prand, MSB first
  uint8_t out[16];
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 128);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&aes);
  return out[15] == addr[0] && out[14] == addr[1] && out[13] == addr[2];
}

static bool rpaMatchesIrk(const uint8_t* addr) {
  if (ahMatches(phoneIRK, addr)) return true;   // IRK as stored
  uint8_t rev[16];
  for (int i = 0; i < 16; i++) rev[i] = phoneIRK[15 - i];
  return ahMatches(rev, addr);                  // reversed byte order
}

static void onPhoneSighted(int rssi) {
  if (rssi < cfgRssiMin) return;   // too faint to count as "here"
  lastPhoneRssi = rssi;
  uint32_t now = millis();
  bool arriving = !phonePresent;   // covers boot (unknown) and away-timeout
  lastSeenMs = now;
  everSeen = true;
  phonePresent = true;
  if (arriving && cfgArrivalWake) {
    sendMagicPacket();             // wake on an awake PC is a harmless no-op
    cntArrivalWakes++;
  }
}

// ---- BLE callbacks -------------------------------------------------------
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    cntAdv++;
    const NimBLEAddress a = d->getAddress();
    const uint8_t* v = a.getVal();
    uint8_t t = a.getType();
    if (t < 4) cntByType[t]++;
    // Path 1: the stack already resolved a bonded peer's RPA — the report
    // carries the identity address we captured at pairing.
    if (haveIdAddr && memcmp(v, idAddr, 6) == 0) {
      cntIdMatch++;
      onPhoneSighted(d->getRSSI());
      return;
    }
    // Path 2: unresolved RPA — resolve manually against the stored IRK.
    if (!haveIRK) return;
    if (t != BLE_ADDR_RANDOM) return;
    if ((v[5] & 0xC0) != 0x40) return;          // not an RPA
    cntRpa++;
    memcpy(rpaRing[rpaRingPos % 8], v, 6);
    rpaRingPos++;
    if (rpaMatchesIrk(v)) {
      cntMatch++;
      onPhoneSighted(d->getRSSI());
    }
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    NimBLEDevice::getScan()->start(0, false, true);  // never stop scanning
  }
};
static ScanCB scanCB;

static void startPresenceScan() {
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&scanCB, false);
  s->setActiveScan(false);        // passive: never transmit, just listen
  s->setInterval(300);            // lazy duty cycle so WiFi HTTP stays snappy
  s->setWindow(40);
  s->setDuplicateFilter(false);   // every sighting must refresh lastSeen
  s->setMaxResults(0);            // callback-only, store nothing
  s->start(0, false, true);
}

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    // Demand pairing on any connection — pops the bond dialog on the
    // phone even when its settings app hides us (Samsung One UI filter).
    NimBLEDevice::startSecurity(info.getConnHandle());
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    // Bonding done — pull the peer's IRK out of the NimBLE bond store.
    const NimBLEAddress id = info.getIdAddress();
    ble_store_key_sec k = {};
    k.peer_addr.type = id.getType();
    memcpy(k.peer_addr.val, id.getVal(), 6);
    ble_store_value_sec v = {};
    if (ble_store_read_peer_sec(&k, &v) == 0 && v.irk_present) {
      memcpy(phoneIRK, v.irk, 16);
      haveIRK = true;
      prefs.putBytes("irk", phoneIRK, 16);
    }
    memcpy(idAddr, id.getVal(), 6);
    haveIdAddr = true;
    prefs.putBytes("ida", idAddr, 6);
    NimBLEDevice::getServer()->disconnect(info.getConnHandle());
  }
};
static SrvCB srvCB;

static bool openPairingWindow() {
  NimBLEDevice::getScan()->stop();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  // Android's pairing screen only lists devices that advertise as general
  // discoverable with a name — set both explicitly, plus a scan response.
  NimBLEAdvertisementData ad;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.setName("aurora");
  adv->setAdvertisementData(ad);
  NimBLEAdvertisementData sr;
  sr.setName("aurora");
  adv->setScanResponseData(sr);
  bool ok = adv->start();
  pairingOpen = ok;
  pairWindowEnd = millis() + PAIR_WINDOW_MS;
  return ok;
}

static void closePairingWindow() {
  NimBLEDevice::getAdvertising()->stop();
  pairingOpen = false;
  startPresenceScan();
}

// ---- HTTP handlers -------------------------------------------------------
static bool tokenOk() {
  return server.hasArg("token") && server.arg("token") == WAKE_TOKEN;
}

void handleWake() {
  if (!tokenOk()) { server.send(403, "text/plain", "no"); return; }
  sendMagicPacket();
  server.send(200, "text/plain",
              ETH.linkUp() ? "magic packet sent"
                           : "sent, but ETH link is DOWN - check cable");
}

void handlePair() {
  if (!tokenOk()) { server.send(403, "text/plain", "no"); return; }
  if (openPairingWindow()) {
    server.send(200, "text/plain",
                "pairing window open for 2 min - pair 'aurora' from the "
                "phone's Bluetooth settings now");
  } else {
    server.send(500, "text/plain", "advertising failed to start");
  }
}

void handleForget() {
  if (!tokenOk()) { server.send(403, "text/plain", "no"); return; }
  NimBLEDevice::deleteAllBonds();
  prefs.remove("irk");
  prefs.remove("ida");
  haveIRK = false;
  haveIdAddr = false;
  phonePresent = false;
  everSeen = false;
  server.send(200, "text/plain", "bonds and IRK wiped - /pair to re-pair");
}

static String hexBytes(const uint8_t* b, size_t n) {
  String s;
  for (size_t i = 0; i < n; i++) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", b[i]);
    s += buf;
  }
  return s;
}

void handleConfig() {
  if (!tokenOk()) { server.send(403, "text/plain", "no"); return; }
  if (server.hasArg("away_s")) {
    long v = server.arg("away_s").toInt();
    if (v >= 30 && v <= 86400) { cfgAwayS = v; prefs.putUInt("away_s", cfgAwayS); }
  }
  if (server.hasArg("arrival_wake")) {
    cfgArrivalWake = server.arg("arrival_wake").toInt() != 0;
    prefs.putBool("arr_wake", cfgArrivalWake);
  }
  if (server.hasArg("rssi_min")) {
    long v = server.arg("rssi_min").toInt();
    if (v >= -100 && v <= 0) { cfgRssiMin = v; prefs.putInt("rssi_min", cfgRssiMin); }
  }
  String s = "away_s: " + String(cfgAwayS) + "\n";
  s += "arrival_wake: " + String(cfgArrivalWake ? 1 : 0) + "\n";
  s += "rssi_min: " + String(cfgRssiMin) + "\n";
  server.send(200, "text/plain", s);
}

void handleDebug() {
  if (!tokenOk()) { server.send(403, "text/plain", "no"); return; }
  String s = "irk: " + (haveIRK ? hexBytes(phoneIRK, 16) : String("none")) + "\n";
  s += "id_addr: " + (haveIdAddr ? hexBytes(idAddr, 6) : String("none")) + "\n";
  s += "types: pub=" + String(cntByType[0]) + " rand=" + String(cntByType[1]) +
       " pub_id=" + String(cntByType[2]) + " rand_id=" + String(cntByType[3]) +
       " id_match=" + String(cntIdMatch) + "\n";
  uint8_t n = rpaRingPos < 8 ? rpaRingPos : 8;
  for (uint8_t i = 0; i < n; i++)
    s += "rpa: " + hexBytes(rpaRing[i], 6) + "\n";
  server.send(200, "text/plain", s);
}

void handleStatus() {
  uint32_t now = millis();
  String s = "aurora up\n";
  s += "ver: " + String(FW_VERSION) + "\n";
  s += "wifi: " + WiFi.localIP().toString() + "\n";
  s += "rssi: " + String(WiFi.RSSI()) + " dBm\n";
  s += "eth link: " + String(ETH.linkUp() ? "up" : "down") + "\n";
  s += "heap: " + String(ESP.getFreeHeap()) + " free\n";
  if (pairingOpen) {
    s += "phone: pairing window open (" +
         String((pairWindowEnd - now) / 1000) + "s left)\n";
  } else if (!haveIRK) {
    s += "phone: not paired (/pair to set up)\n";
  } else if (!everSeen) {
    s += "phone: paired, never seen since boot\n";
  } else {
    s += "phone: " + String(phonePresent ? "present" : "away");
    s += " (last seen " + String((now - lastSeenMs) / 1000) + "s ago, ";
    s += String(lastPhoneRssi) + " dBm)\n";
  }
  s += "motion: no sensor\n";
  s += "ble: adv=" + String(cntAdv) + " rpa=" + String(cntRpa) +
       " match=" + String(cntMatch) + " id_match=" + String(cntIdMatch) +
       " arrival_wakes=" + String(cntArrivalWakes) + "\n";
  server.send(200, "text/plain", s);
}

void setup() {
  Serial.begin(115200);

  prefs.begin("aurora");
  if (prefs.getBytesLength("irk") == 16) {
    prefs.getBytes("irk", phoneIRK, 16);
    haveIRK = true;
  }
  if (prefs.getBytesLength("ida") == 6) {
    prefs.getBytes("ida", idAddr, 6);
    haveIdAddr = true;
  }
  cfgAwayS       = prefs.getUInt("away_s", cfgAwayS);
  cfgArrivalWake = prefs.getBool("arr_wake", cfgArrivalWake);
  cfgRssiMin     = prefs.getInt("rssi_min", cfgRssiMin);

  ETH.begin();
  ETH.config(ETH_IP, ETH_GW, ETH_MASK);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("aurora");      // name shown by the router's device list
  WiFi.setSleep(false);            // keep HTTP latency low
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.printf("\nWiFi: %s\n", WiFi.localIP().toString().c_str());

  ArduinoOTA.setHostname("aurora");
  ArduinoOTA.setPassword(WAKE_TOKEN);  // reuse; or set a separate one
  ArduinoOTA.begin();

  server.on("/wake", handleWake);
  server.on("/status", handleStatus);
  server.on("/pair", handlePair);
  server.on("/forget", handleForget);
  server.on("/debug", handleDebug);
  server.on("/config", handleConfig);
  server.begin();

  // BLE last, once the wake path is already serving.
  NimBLEDevice::init("aurora");
  NimBLEDevice::setSecurityAuth(true /*bond*/, false /*mitm*/, true /*sc*/);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(&srvCB);
  // Bond may predate this build — recover the identity address from the store.
  if (!haveIdAddr && NimBLEDevice::getNumBonds() > 0) {
    NimBLEAddress b = NimBLEDevice::getBondedAddress(0);
    memcpy(idAddr, b.getVal(), 6);
    haveIdAddr = true;
    prefs.putBytes("ida", idAddr, 6);
  }
  startPresenceScan();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();

  uint32_t now = millis();
  if (pairingOpen && (int32_t)(now - pairWindowEnd) >= 0) closePairingWindow();
  if (phonePresent && now - lastSeenMs > cfgAwayS * 1000UL) phonePresent = false;
}
