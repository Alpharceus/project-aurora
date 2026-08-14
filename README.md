# Aurora — Wake-on-LAN over WiFi with a WT32-ETH01

Wake a sleeping PC from anywhere on your WiFi (and later, from anywhere at all)
using an inexpensive ESP32 board with a built-in Ethernet jack.

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
arduino-cli compile --fqbn esp32:esp32:wt32-eth01 aurora_wake
```

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
arduino-cli upload --fqbn esp32:esp32:wt32-eth01 -p COM7 aurora_wake
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
arduino-cli compile --fqbn esp32:esp32:wt32-eth01 aurora_wake
arduino-cli upload --fqbn esp32:esp32:wt32-eth01 -p <board-ip> --upload-field password=<token> aurora_wake
```

On Windows you must allow espota inbound through the firewall once (the
*board* connects back to the *PC* to pull the binary):

```
netsh advfirewall firewall add rule name="ESP32 OTA (espota)" dir=in action=allow program="%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\<ver>\tools\espota.exe" enable=yes
```

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

## Security model

- The token is the only gate, over plain HTTP, LAN-only. Accepted trade:
  the worst a leaked token allows is *waking the PC*.
- Nothing is exposed to the internet — no port forwarding. Remote wake goes
  through a Tailscale-connected relay host inside the LAN (a Pi, in our
  case) that curls the board.
- The board's ETH subnet is unreachable from the LAN (point-to-point island);
  a failed connect to it from the LAN is correct behavior.

## Roadmap

- Pi relay: one-line curl over Tailscale → full off-LAN wake.
- Android app so the token lives somewhere better than a browser bookmark.
