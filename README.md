# Photon DS18B20 → MQTT → Home Assistant

Particle Photon firmware that reads a DS18B20 1-Wire temperature sensor and
publishes the temperature to a Mosquitto MQTT broker. The sensor appears
automatically in Home Assistant via MQTT discovery as a "Photon DS18B20"
device with an availability (online/offline) indicator.

## Wiring

| DS18B20 lead | Photon |
|--------------|--------|
| red (VDD)    | 3V3    |
| black (GND)  | GND    |
| yellow (DQ)  | D0     |

**A 4.7 kΩ resistor between D0 and 3V3 is required.** 1-Wire is an open-drain
bus: the sensor only ever pulls the line low, so without a pull-up the line
never returns high and the Photon sees no sensor at all. Small DS18B20 breakout
boards usually have the resistor fitted already; bare probes (the waterproof
stainless ones especially) do not.

The firmware enables the Photon's internal pull-up as a fallback. At roughly
40 kΩ it is far weaker than the datasheet's 4.7 kΩ and only tends to work with
a very short lead — treat it as a way to bench-test before the resistor
arrives, not as a substitute for it.

Powering VDD from 3V3 (rather than leaving it grounded for parasite power) is
what lets the firmware poll the bus to detect the end of a conversion.

## Configure

Copy the credentials template and fill in your values (the copy is gitignored):

```sh
cp src/secrets.h.example src/secrets.h
```

- `MQTT_HOST` / `MQTT_PORT` — your Mosquitto broker (IP is easiest)
- `MQTT_USER` / `MQTT_PASS` — leave empty for anonymous access

Non-secret settings are at the top of `src/photon-ds18b20-mqtt.cpp`:

- `PUBLISH_INTERVAL_MS` — how often to read and publish (default 30 s)
- `ONEWIRE_PIN` — data line, default `D0`
- `TEMPERATURE_OFFSET_C` — fixed correction added to the published temperature
- `WATCHDOG_TIMEOUT_MS` — reboot if `loop()` stalls this long (default 60 s)

Wi-Fi credentials are not in the firmware; the Photon uses its stored
credentials (set them once with `particle serial wifi` or the Particle app).

## No Particle cloud

The firmware runs `SYSTEM_MODE(SEMI_AUTOMATIC)` and never calls
`Particle.connect()`, so the device brings up Wi-Fi itself and talks only to
your MQTT broker — it does not connect to api.particle.io. The trade-off is
that OTA flashing is unavailable; flash locally over DFU (see below).

## Recovering from hangs

The device is unattended, so every failure path has to self-heal:

- A failed sensor read (no presence pulse, a bad scratchpad CRC, a conversion
  that never completes) releases the bus and forces the full init sequence —
  reset, `READ ROM`, resolution write — on the next cycle. Without that, one
  brown-out or a briefly unplugged probe would silence the device until it was
  power-cycled.
- An `ApplicationWatchdog` reboots the Photon if `loop()` stops checking in for
  `WATCHDOG_TIMEOUT_MS`. This covers hangs the application cannot detect from
  the inside — notably `MQTT::readByte()` in the vendored client, which spins
  forever waiting on bytes if the TCP connection dies mid-packet.

The failure signature for both is the same from Home Assistant's side: the
value freezes at whatever was last published and "last updated" keeps aging,
because HA holds the last retained value it received. A flat line is a stalled
publisher, not a stuck sensor.

Two failure modes get explicit checks because they would otherwise look like
valid data: an all-zero scratchpad (data line shorted to ground) has a CRC of
zero and so passes the CRC check while reading as a plausible 0.00 °C, and two
sensors on the bus answer `READ ROM` simultaneously, returning the bitwise AND
of both ROM codes — caught by the ROM CRC.

## Build (docker buildpack, works on Apple Silicon)

```sh
./build.sh
```

Output: `target/firmware.bin`. The image is x86 and runs under Rosetta via
`--platform linux/amd64`; compilation works fine that way.

Device OS 2.3.1 is the last LTS line supporting the Photon (Gen 2), hence the
`2.3.1-photon` buildpack tag.

## Flash

The firmware requires Device OS ≥ 2.3.x on the Photon. Older Photons usually
run 0.x/1.x, so update once first:

```sh
npm install -g particle-cli   # or: brew install particle-cli
# put the Photon in DFU mode: hold SETUP, tap RESET, release SETUP when blinking yellow
particle update               # brings Device OS to the latest 2.3.x
particle flash --local target/firmware.bin   # device in DFU mode again
```

OTA flashing (`particle flash <device-name>`) does not work with this firmware:
it stays off the Particle cloud by design, so the device is never online for
the cloud to push to. Use the local DFU flash above.

## Home Assistant

Requirements on the HA side:

1. Mosquitto broker running and the MQTT integration configured in HA.
2. MQTT discovery enabled with the default `homeassistant` prefix (it is by
   default).

On every MQTT connect the firmware publishes a retained discovery config to
`homeassistant/sensor/photon_ds18b20_<deviceid>/temperature/config`. The device
then shows up under **Settings → Devices & Services → MQTT** as
"Photon DS18B20" with one temperature entity (°C).

State is published as JSON every 30 s to
`particle/photon_ds18b20_<deviceid>/state`; availability (`online`/`offline`,
with an MQTT last-will) to `particle/photon_ds18b20_<deviceid>/status`.

Debug from any machine with mosquitto clients installed:

```sh
mosquitto_sub -h <broker> -v -t 'particle/#' -t 'homeassistant/#'
```

USB serial logs (`particle serial monitor` or any terminal at 9600 baud) show
the sensor ROM code, MQTT connection state and each published payload.

### Coming from the BME680 version

The node ID changed from `photon_bme680_<deviceid>` to
`photon_ds18b20_<deviceid>`, so the old device and its four entities linger in
Home Assistant, permanently unavailable — the retained discovery configs are
still on the broker and nothing clears them. Delete them by publishing an empty
retained payload to each:

```sh
for s in temperature humidity pressure gas_resistance; do
  mosquitto_pub -h <broker> -r -n \
    -t "homeassistant/sensor/photon_bme680_<deviceid>/$s/config"
done
mosquitto_pub -h <broker> -r -n -t "particle/photon_bme680_<deviceid>/state"
mosquitto_pub -h <broker> -r -n -t "particle/photon_bme680_<deviceid>/status"
```

## Layout

- `src/photon-ds18b20-mqtt.cpp` — application, including a bit-banged 1-Wire
  master and the DS18B20 commands (no sensor library to vendor)
- `lib/MQTT` — hirotakaster's MQTT client library for Particle (vendored)

## License

MIT (see `LICENSE`). The vendored library keeps its own license: `lib/MQTT` is
MIT (Hirotaka Niisato / Nicholas O'Leary).

## Notes

- 1-Wire is bit-banged in software. Each bit slot disables interrupts for at
  most ~70 µs to keep the edges within the DS18B20's timing windows; that is
  short enough not to disturb Wi-Fi or the system thread.
- The sensor is configured for 12-bit resolution (0.0625 °C steps), which costs
  a 750 ms conversion. Because VDD is externally powered, the firmware polls
  read slots to learn when the conversion has finished rather than blindly
  waiting the full time.
- The firmware assumes a single sensor and addresses it with `SKIP ROM`. Its
  ROM code is read once at init and logged, purely so the serial log identifies
  which probe is attached. Supporting several sensors would need a `SEARCH ROM`
  walk and `MATCH ROM` addressing.
- Unlike the BME680, the DS18B20 has no meaningful self-heating, so the
  published value tracks ambient directly. `TEMPERATURE_OFFSET_C` is kept for
  calibrating out probe-to-probe variation (±0.5 °C over -10…+85 °C).
