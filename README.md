# Aurora — Wake-on-LAN over WiFi with a WT32-ETH01

Wake a sleeping PC from anywhere on your WiFi (and later, from anywhere at all)
using an inexpensive ESP32 board with a built-in Ethernet jack. Since v2 the
same board also detects your phone's presence over BLE — walk in the door
after being away and the tower wakes itself, no app running on the phone.

```
phone/laptop → WiFi → HTTP /wake?token=… → WT32-ETH01 → magic packet → PC's wired NIC
```

The WT32-ETH01 (ESP32 + LAN8720 PHY) runs both network stacks at once:
its **WiFi** side joins your home network and serves two authenticated HTTP
endpoints; its **Ethernet** side is cabled point-to-point into the PC's wired
NIC and is the only place the magic packet ever goes.

## Why this shape

- **The PC's WiFi can't do it.** Most consumer WiFi NICs have no working
  WoWLAN. The wake packet must arrive on the wired NIC.
- **Point-to-point, no switch needed.** The PC's Ethernet port connects
  directly to the board. No DHCP, no IP config on the PC side — a sleeping
  NIC matches the magic packet **in hardware at layer 2**; it never consults
  the IP stack.
- **The static-subnet trick.** The firmware pins a static subnet
  (`192.168.50.1/24`) to the ETH interface and sends the wake packet to that
  subnet's broadcast address. That forces the packet out the wire instead of
  the WiFi interface. (Stock ESPHome can't run WiFi + ETH simultaneously —
  hence plain Arduino.)
- **10/100 is fine.** NICs renegotiate down to 10/100 in sleep anyway.

## Hardware

What we actually used:

- [WT32-ETH01 board](https://www.amazon.com/dp/B0HB3B557Z) (ESP32 + LAN8720
  Ethernet)
- [CP2102 USB-UART dongles, 2-pack](https://www.amazon.com/dp/B0GWVV83SK) —
  one for the first (serial) flash, one living in a USB wall charger as the
  permanent power supply. After the first flash, updates go over WiFi (OTA)
  and the data dongle retires.
- Jumper wires, one Ethernet cable, a USB wall charger.

## Setup

### 1. Prepare the firmware

```
copy aurora_wake.example.ino aurora_wake/aurora_wake.ino
```

(The folder must be named `aurora_wake` — Arduino tooling requires the sketch
directory to match the sketch name.) Fill in the four values at the top:
2.4 GHz SSID + password, a long random token, and the PC's wired-NIC MAC
(`getmac /v` on Windows). **Never commit the filled-in copy** — this repo's
`.gitignore` is default-deny for that reason.

### 2. Arm the PC's NIC (Windows)

- Device Manager → the wired NIC → Power Management: allow this device to
  wake the computer, **magic packet only**.
- Advanced tab (or `Set-NetAdapterAdvancedProperty`): *Wake on Magic Packet*
  = Enabled, *Wake on pattern match* = **Disabled** (pattern wake causes
  spurious wakeups), *Shutdown Wake-On-Lan* = Enabled.
- Verify: `powercfg /devicequery wake_armed` must list the NIC.
- Disarm wake sources you don't want (we disarmed the WiFi NIC:
  `powercfg /devicedisablewake "<device name>"`), so wake attribution stays
  clean.
- Check Fast Startup is off (`HiberbootEnabled = 0`) — it breaks
  WoL-from-shutdown semantics on many systems.
- If the Ethernet port has a leftover static IP, remove it. The link needs
  **no** PC-side IP config at all, and less surface is better.

### 3. Toolchain

```
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install NimBLE-Arduino
arduino-cli compile --fqbn esp32:esp32:wt32-eth01:PartitionScheme=min_spiffs aurora_wake
```

Two dependencies: the esp32 core (bundles `ETH.h`, `WiFi.h`, `WiFiUdp.h`,
`WebServer.h`, `ArduinoOTA.h`, `Preferences.h`, mbedtls) and NimBLE-Arduino
for the v2 presence scanning.

**The partition scheme matters and cannot change over OTA.** The default
scheme has 1.25MB app slots; the v2 sketch is ~1.29MB and won't fit. Set
`PartitionScheme=min_spiffs` (1.9MB slots) at the **first serial flash** —
if you flashed with defaults, you get exactly one more date with the UART
dongle. (Don't trust the compile output's "Maximum is 8388608 bytes" under
the default scheme — that's a boards.txt bug; the real slot is 1.25MB.)

### 4. First flash (serial — the board has no USB port)

Wiring, with the board **unpowered**:

| Dongle (data)      | WT32-ETH01                  |
|--------------------|-----------------------------|
| TX                 | **RX0** (not RXD!)          |
| RX                 | **TX0** (not TXD!)          |
| GND                | GND (any GND pin)           |
| 5V                 | **5V pin** (see gotcha #3)  |

Then: jumper **IO0 → GND**, apply power (IO0 is sampled only at power-on),
and:

```
arduino-cli upload --fqbn esp32:esp32:wt32-eth01:PartitionScheme=min_spiffs -p COM7 aurora_wake
```

Afterward: power off, remove the IO0 jumper, power on. Watch the serial line
at 115200 baud — you should see boot ROM output, dots while WiFi connects,
then the IP.

### 5. Smoke test

- `http://<board-ip>/status` → `aurora up`, firmware version, WiFi IP,
  `eth link: up/down`.
- `/wake` with no/wrong token → 403. With the token → 200 and the packet
  fires (it also self-reports if the ETH link is down).
- Give the board a **DHCP reservation** in your router so the IP never
  drifts.

### 6. Ethernet leg

Cable the board to the PC's port. `/status` should report `eth link: up` —
**including while the PC sleeps** (the NIC keeps the PHY alive). If the link
drops in sleep, revisit the NIC power settings; that's the main failure mode.

### 7. OTA from then on

```
arduino-cli compile --fqbn esp32:esp32:wt32-eth01:PartitionScheme=min_spiffs aurora_wake
arduino-cli upload --fqbn esp32:esp32:wt32-eth01:PartitionScheme=min_spiffs -p <board-ip> --upload-field password=<token> aurora_wake
```

On Windows you must allow espota inbound through the firewall once (the
*board* connects back to the *PC* to pull the binary):

```
netsh advfirewall firewall add rule name="ESP32 OTA (espota)" dir=in action=allow program="%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\<ver>\tools\espota.exe" enable=yes
```

## v2: phone presence (BLE), and the auto-wake

The board passively recognizes your phone via **IRK resolution**: pair once,
and it resolves the phone's randomized Bluetooth addresses forever — no app,
no beacon, no battery cost on the phone. Arrive home after ≥10 minutes away
and the board fires the magic packet by itself.

Setup (once):

1. `GET /pair?token=…` — opens a 2-minute pairing window.
2. Pair from the phone. **Samsung/One UI hides generic BLE devices** in its
   Bluetooth-settings list, so use a BLE app (e.g. nRF Connect): scan →
   connect to `aurora` → the board demands security → Android pops the
   standard pair dialog → accept. Done; the bond survives OTA updates.
3. Watch `GET /status` — `phone: present (last seen Ns ago, X dBm)`.

Tuning via `GET /config?token=…` (persisted in NVS): `away_s` (seconds
unseen before "away", default 600), `arrival_wake` (0/1), `rssi_min`
(ignore sightings weaker than this dBm).

`GET /forget?token=…` wipes bonds for re-pairing.

Two findings worth stealing for your own build:

- **You may never see a resolvable RPA in your scan callback.** NimBLE loads
  bonded-peer IRKs into its resolving machinery, so the phone's
  advertisements arrive already resolved, bearing the *identity address*
  from pairing. Match on that identity address; keep manual `ah()` AES
  resolution only as a fallback (ours has never once fired).
- **An idle Samsung phone advertises plenty** (SmartThings Find, Fast Pair):
  presence detection worked with the phone asleep in a pocket, no app
  installed. Reports of "Android doesn't advertise when idle" did not hold
  for a current Samsung flagship.

## Every problem we actually hit (learn from our afternoon)

1. **CP2102 shows "Error, code 28" in Device Manager** — driver not
   installed. Get the Silicon Labs *CP210x Universal Windows Driver*; then it
   enumerates as a COM port.
2. **`Failed to connect to ESP32: No serial data received`** — we had the
   wires on the **TXD/RXD** pins. The boot UART is **TX0/RX0**. Crossed or
   wrong-pin serial is the cause of nearly all "dead board" reports; suspect
   wiring before hardware.
3. **The dongle's 3.3V pin cannot power this board.** The CP2102's internal
   regulator supplies ~100 mA; the ESP32 + PHY spike far beyond that when the
   radio starts. It's sneaky: **flashing works** (radio off = low current),
   then the firmware browns out invisibly at boot. Power the **5V pin** from
   USB bus power. "3.3V" in flashing guides refers to *logic levels*, not
   the power rail. And never feed 5V into the 3.3V pin.
4. **Total serial silence + no WiFi + Ethernet link down** = the board simply
   has no power. Check the red LED before debugging anything clever. (We
   spent a capture window discovering an unplugged wall dongle.)
5. **IO0 is sampled only at power-on.** Jumpering it after power is applied
   does nothing. Jumper first, then power. Any GND pin works — grounds are
   one net; the jumper doesn't need "its own" ground.
6. **Wrong WiFi credentials look identical to a dead board** — `setup()`
   blocks forever before the web server starts. The serial line (endless
   dots) is what distinguishes them.
7. **ESP32 is 2.4 GHz-only.** If your router band-steers one SSID across
   2.4/5/6 GHz, fine — but don't rename or remove the 2.4 GHz side, or the
   board is orphaned (and OTA needs WiFi: recovery means serial reflash).
   WPA3-Personal worked fine with arduino-esp32 core 3.x.
8. **Windows can't "see" the wake packets with a normal socket.** The IP
   stack drops the foreign-subnet broadcast as *not locally destined* —
   by design. To verify egress, capture at packet level (elevated):
   `pktmon filter add wol -t UDP -p 9; pktmon start --capture; …trigger…;
   pktmon stop; pktmon etl2txt …` (some pktmon builds lack `etl2pcapng`).
   Assert: 102-byte UDP broadcast, source = the board's **ETH** MAC, and no
   copy on the WiFi adapter.
9. **The NIC needs time to settle into sleep.** Entering sleep, the NIC
   renegotiates the link down to 10 Mbps; a magic packet fired during that
   window (~first 30–60 s) is silently lost. **Wait ≥60 s after sleep before
   firing** — or send the packet twice, ~10 s apart. This exact race cost us
   a test round; it will look like "WoL is flaky" when it's just timing.
10. **Wake attribution can lie.** On our ASUS board, `powercfg /lastwake`
    reports a genuine WoL wake as ACPI **"Power Button"**, while a keyboard
    wake shows as "PCI Express Root Port". Verify wakes by timing correlation
    with hands off the hardware, not by the reported device name.
11. **OTA fails with "No response from device"** after successful auth =
    Windows Firewall blocking espota's inbound transfer (see step 7).
12. **`--upload-field` belongs to `arduino-cli upload`**, not `compile` —
    two-step it when doing OTA.
13. **"aurora" won't appear in Samsung's Bluetooth pairing list.** One UI
    filters out generic BLE peripherals. Pair through nRF Connect instead,
    with the firmware calling `startSecurity()` on connect so the bond
    dialog pops regardless.
14. **`match=0` doesn't mean your IRK math is wrong.** The stack resolves
    bonded RPAs before your callback sees them — check for the identity
    address before debugging AES byte orders (we brute-forced four
    orderings before realizing the packets were pre-resolved).
15. **The away-hysteresis bites during testing:** Bluetooth must be off for
    the full `away_s` (default 10 min) before re-enabling it counts as an
    arrival. Shorter toggles do nothing, by design.
16. **The Windows-side WiFi band can shuffle after sleep cycles** (2.4 ↔
    5/6 GHz on band-steered SSIDs) — unrelated to the board, but it will
    confuse your network debugging if you don't know to look.

## Security model

- The token is the only gate, over plain HTTP, LAN-only. Accepted trade:
  the worst a leaked token allows is *waking the PC*.
- Nothing is exposed to the internet — no port forwarding. Remote wake goes
  through a Tailscale-connected relay host inside the LAN (a Pi, in our
  case) that curls the board.
- The board's ETH subnet is unreachable from the LAN (point-to-point island);
  a failed connect to it from the LAN is correct behavior.

## Roadmap

- LD2410 mmWave motion over UART (body-presence, complements the phone
  signal) — sensor TX → an input-only pin (35/36/39), **never IO2** (it's a
  boot-strap pin and the sensor's idle-high TX blocks the serial bootloader).
- Pi relay: one-line curl over Tailscale → full off-LAN wake. The relay
  script should fire twice ~10 s apart (gotcha #9).
- Android app: wake button + status + token storage (presence needs nothing
  on the phone, so the app stays thin).
