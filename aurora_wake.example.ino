/*
 * Aurora — WT32-ETH01 wake device
 * WiFi (STA) carries the trigger; magic packet exits the Ethernet jack
 * point-to-point into the PC's wired NIC.
 *
 * Board: WT32-ETH01 (arduino-esp32 core; select "WT32-ETH01" in the IDE)
 * First flash over UART (IO0 low at power-on); afterwards OTA over WiFi.
 *
 * SETUP: copy this file to aurora_wake/aurora_wake.ino (the folder name
 * must match the sketch name for arduino-cli) and fill in the four
 * values below. Never commit the filled-in copy.
 */

// ---------- fill these in ----------
const char* WIFI_SSID   = "YOUR_SSID";        // 2.4 GHz network (ESP32 has no 5/6 GHz radio)
const char* WIFI_PASS   = "YOUR_PASS";
const char* WAKE_TOKEN  = "pick-a-long-random-string";
// Target PC's wired NIC MAC, e.g. from `getmac /v` (format: AA:BB:CC:DD:EE:FF)
const uint8_t PC_MAC[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
const char* FW_VERSION  = "1.1";
// -----------------------------------

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

// Point-to-point subnet on the wire. Nothing else lives here; the
// broadcast address pins the packet's route to the ETH interface.
IPAddress ETH_IP(192, 168, 50, 1);
IPAddress ETH_GW(192, 168, 50, 1);
IPAddress ETH_MASK(255, 255, 255, 0);
IPAddress ETH_BCAST(192, 168, 50, 255);

WebServer server(80);
WiFiUDP udp;

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

void handleWake() {
  if (!server.hasArg("token") || server.arg("token") != WAKE_TOKEN) {
    server.send(403, "text/plain", "no");
    return;
  }
  sendMagicPacket();
  server.send(200, "text/plain",
              ETH.linkUp() ? "magic packet sent"
                           : "sent, but ETH link is DOWN - check cable");
}

void handleStatus() {
  String s = "aurora up\n";
  s += "ver: " + String(FW_VERSION) + "\n";
  s += "wifi: " + WiFi.localIP().toString() + "\n";
  s += "eth link: " + String(ETH.linkUp() ? "up" : "down") + "\n";
  server.send(200, "text/plain", s);
}

void setup() {
  Serial.begin(115200);

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
  server.begin();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
}
