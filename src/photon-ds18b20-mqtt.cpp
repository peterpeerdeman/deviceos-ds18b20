/*
 * DS18B20 -> MQTT -> Home Assistant (auto-discovery) on a Particle Photon.
 *
 * Wiring (1-Wire, typical waterproof probe colours):
 *   DS18B20 red (VDD)  -> 3V3
 *   DS18B20 black (GND)-> GND
 *   DS18B20 yellow (DQ)-> D0
 *
 * A 4.7k pull-up resistor between D0 and 3V3 is required. The Photon's
 * internal pull-up (~40k) is enabled as a fallback and can work with a short
 * lead, but it is too weak to be reliable on a metre of probe cable.
 */

#include "Particle.h"
#include "MQTT.h"

// SEMI_AUTOMATIC keeps the device off the Particle cloud: Wi-Fi is brought up
// by the application and Particle.connect() is never called. Note this also
// disables OTA flashing - use `particle flash --local` over DFU.
SYSTEM_MODE(SEMI_AUTOMATIC);
SYSTEM_THREAD(ENABLED);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
// Broker address and credentials live in src/secrets.h (gitignored);
// copy src/secrets.h.example to src/secrets.h and fill in your values.
#include "secrets.h"

const uint32_t PUBLISH_INTERVAL_MS = 30000;

// 1-Wire data line (DS18B20 yellow lead).
const pin_t ONEWIRE_PIN = D0;

// If loop() stops running this long, reboot. Worst case for one iteration is a
// failing MQTT connect (TCP connect plus a 15 s wait for CONNACK) on top of a
// 750 ms conversion, so this leaves ample headroom while still recovering from
// a hang within a minute.
const uint32_t WATCHDOG_TIMEOUT_MS = 60000;

// Fixed correction applied to the published temperature, in degrees Celsius.
// Leave at 0 and correct in Home Assistant instead if you prefer.
const float TEMPERATURE_OFFSET_C = 0.0f;

// ---------------------------------------------------------------------------
// MQTT topics
// ---------------------------------------------------------------------------
String nodeId;            // "photon_ds18b20_<deviceid>"
String stateTopic;        // particle/<nodeId>/state
String availabilityTopic; // particle/<nodeId>/status

void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
    // No subscriptions; nothing to do.
}

MQTT mqtt(MQTT_HOST, MQTT_PORT, 1024, mqttCallback);

// ---------------------------------------------------------------------------
// 1-Wire bus (bit-banged)
// ---------------------------------------------------------------------------
// The line is open-drain: the master only ever pulls it low, and releasing it
// means switching the pin back to an input so the pull-up restores the high
// level. Interrupts are disabled around each timing-critical edge - a slot is
// tens of microseconds, so this never blocks the system thread for long.

inline void owDriveLow() {
    pinResetFast(ONEWIRE_PIN);
    pinMode(ONEWIRE_PIN, OUTPUT);
}

inline void owRelease() {
    pinMode(ONEWIRE_PIN, INPUT_PULLUP);
}

bool owReset() {
    owRelease();

    // If the bus is still held low, something is wrong (short, or no pull-up).
    uint32_t waited = 0;
    while (!pinReadFast(ONEWIRE_PIN)) {
        if (++waited > 250) {
            return false;
        }
        delayMicroseconds(1);
    }

    noInterrupts();
    owDriveLow();
    interrupts();
    delayMicroseconds(480);

    noInterrupts();
    owRelease();
    delayMicroseconds(70);
    bool present = !pinReadFast(ONEWIRE_PIN); // sensor pulls the line low
    interrupts();

    delayMicroseconds(410);
    return present;
}

void owWriteBit(bool bit) {
    if (bit) {
        noInterrupts();
        owDriveLow();
        delayMicroseconds(10);
        owRelease();
        interrupts();
        delayMicroseconds(55);
    } else {
        noInterrupts();
        owDriveLow();
        delayMicroseconds(65);
        owRelease();
        interrupts();
        delayMicroseconds(5);
    }
}

bool owReadBit() {
    noInterrupts();
    owDriveLow();
    delayMicroseconds(3);
    owRelease();
    delayMicroseconds(10);
    bool bit = pinReadFast(ONEWIRE_PIN);
    interrupts();
    delayMicroseconds(53);
    return bit;
}

void owWrite(uint8_t value) {
    for (uint8_t i = 0; i < 8; i++) {
        owWriteBit((value >> i) & 0x01); // LSB first
    }
}

uint8_t owRead() {
    uint8_t value = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (owReadBit()) {
            value |= (uint8_t)(1 << i);
        }
    }
    return value;
}

// Maxim/Dallas CRC-8, polynomial x^8 + x^5 + x^4 + 1. Bitwise so it costs
// flash rather than a 256-byte table.
uint8_t owCrc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// DS18B20
// ---------------------------------------------------------------------------
const uint8_t CMD_READ_ROM = 0x33;
const uint8_t CMD_SKIP_ROM = 0xCC;
const uint8_t CMD_CONVERT_T = 0x44;
const uint8_t CMD_WRITE_SCRATCHPAD = 0x4E;
const uint8_t CMD_READ_SCRATCHPAD = 0xBE;

const uint8_t DS18B20_FAMILY_CODE = 0x28;
const uint8_t DS18B20_CONFIG_12BIT = 0x7F;

// Datasheet worst case for a 12-bit conversion is 750 ms; allow a margin.
const uint32_t CONVERSION_TIMEOUT_MS = 900;

bool sensorOk = false;
uint8_t romCode[8];

// Only one sensor is expected on the bus, so READ ROM doubles as a presence
// check: with two sensors answering at once the returned bytes are the AND of
// both ROMs and the CRC fails.
bool sensorInit() {
    if (!owReset()) {
        return false;
    }

    owWrite(CMD_READ_ROM);
    for (uint8_t i = 0; i < 8; i++) {
        romCode[i] = owRead();
    }
    if (owCrc8(romCode, 8) != 0) {
        return false;
    }
    if (romCode[0] != DS18B20_FAMILY_CODE) {
        Log.warn("1-Wire family code 0x%02x is not a DS18B20 (0x28)", romCode[0]);
    }

    // Force 12-bit resolution; TH/TL are unused alarm thresholds.
    if (!owReset()) {
        return false;
    }
    owWrite(CMD_SKIP_ROM);
    owWrite(CMD_WRITE_SCRATCHPAD);
    owWrite(0x00); // TH
    owWrite(0x00); // TL
    owWrite(DS18B20_CONFIG_12BIT);

    return true;
}

// Waits out a conversion by polling read slots: with the sensor externally
// powered (red to 3V3) it holds the line low until the conversion finishes.
bool waitForConversion() {
    uint32_t start = millis();
    while (millis() - start < CONVERSION_TIMEOUT_MS) {
        if (owReadBit()) {
            return true;
        }
        delay(10);
    }
    return false;
}

bool sensorRead(float *celsius) {
    if (!owReset()) {
        return false;
    }
    owWrite(CMD_SKIP_ROM);
    owWrite(CMD_CONVERT_T);

    if (!waitForConversion()) {
        return false;
    }

    if (!owReset()) {
        return false;
    }
    owWrite(CMD_SKIP_ROM);
    owWrite(CMD_READ_SCRATCHPAD);

    uint8_t scratchpad[9];
    for (uint8_t i = 0; i < 9; i++) {
        scratchpad[i] = owRead();
    }
    if (owCrc8(scratchpad, 9) != 0) {
        return false;
    }
    // An all-zero scratchpad has a CRC of zero, so it passes the check above
    // while meaning "the bus is shorted to ground", not "0.00 degC".
    bool allZero = true;
    for (uint8_t i = 0; i < 9; i++) {
        if (scratchpad[i] != 0) {
            allZero = false;
            break;
        }
    }
    if (allZero) {
        return false;
    }

    int16_t raw = (int16_t)((uint16_t)scratchpad[1] << 8 | scratchpad[0]);
    *celsius = raw / 16.0f;
    return true;
}

// ---------------------------------------------------------------------------
// Home Assistant MQTT discovery
// ---------------------------------------------------------------------------
void publishDiscovery() {
    const char *key = "temperature";

    char device[192];
    snprintf(device, sizeof(device),
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Photon DS18B20\","
             "\"mf\":\"Particle\",\"mdl\":\"Photon\"}",
             nodeId.c_str());

    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config",
             nodeId.c_str(), key);

    char payload[640];
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Temperature\",\"uniq_id\":\"%s_%s\","
             "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\\u00b0C\","
             "\"stat_cla\":\"measurement\","
             "\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.%s }}\","
             "\"avty_t\":\"%s\",%s}",
             nodeId.c_str(), key, stateTopic.c_str(), key,
             availabilityTopic.c_str(), device);

    mqtt.publish(topic, (const uint8_t *)payload, strlen(payload), true /* retain */);
}

bool mqttConnect() {
    String clientId = "photon-ds18b20-" + System.deviceID();
    const char *user = (MQTT_USER[0] != '\0') ? MQTT_USER : NULL;
    const char *pass = (MQTT_PASS[0] != '\0') ? MQTT_PASS : NULL;

    bool ok = mqtt.connect(clientId.c_str(), user, pass,
                           availabilityTopic.c_str(), MQTT::QOS1, 1 /* will retain */,
                           "offline", true /* clean session */);
    if (ok) {
        mqtt.publish(availabilityTopic.c_str(),
                     (const uint8_t *)"online", strlen("online"), true);
        publishDiscovery();
        Log.info("MQTT connected to %s:%u", MQTT_HOST, MQTT_PORT);
    }
    return ok;
}

// ---------------------------------------------------------------------------

SerialLogHandler logHandler(LOG_LEVEL_INFO);

unsigned long lastPublish = 0;
unsigned long lastConnectAttempt = 0;

// Runs on its own thread, so it still fires when the application thread is
// stuck - including inside MQTT::readByte(), which spins forever waiting on
// bytes that a half-dead TCP connection will never deliver.
void onWatchdogTimeout() {
    System.reset();
}

ApplicationWatchdog watchdog(WATCHDOG_TIMEOUT_MS, onWatchdogTimeout, 1536);

void logRomCode() {
    Log.info("DS18B20 found: %02x%02x%02x%02x%02x%02x%02x%02x",
             romCode[0], romCode[1], romCode[2], romCode[3],
             romCode[4], romCode[5], romCode[6], romCode[7]);
}

void setup() {
    String id = System.deviceID();
    nodeId = "photon_ds18b20_" + id;
    stateTopic = "particle/" + nodeId + "/state";
    availabilityTopic = "particle/" + nodeId + "/status";

    WiFi.on();
    WiFi.connect();

    owRelease();

    sensorOk = sensorInit();
    if (sensorOk) {
        logRomCode();
    } else {
        Log.error("DS18B20 not responding on D0 - check wiring and pull-up");
    }
}

void loop() {
    ApplicationWatchdog::checkin();

    if (!WiFi.ready() && !WiFi.connecting()) {
        WiFi.connect();
    }

    if (mqtt.isConnected()) {
        mqtt.loop();
    } else if (WiFi.ready() && millis() - lastConnectAttempt > 5000) {
        lastConnectAttempt = millis();
        if (!mqttConnect()) {
            Log.warn("MQTT connect failed, retrying in 5s");
        }
    }

    if (lastPublish != 0 && millis() - lastPublish < PUBLISH_INTERVAL_MS) {
        return;
    }
    lastPublish = millis();

    // Retry sensor init, whether the sensor was missing at boot or a read
    // failure below cleared the flag.
    if (!sensorOk) {
        sensorOk = sensorInit();
        if (!sensorOk) {
            Log.error("DS18B20 not responding on D0 - check wiring and pull-up");
            return;
        }
        logRomCode();
    }

    float celsius = 0.0f;
    if (!sensorRead(&celsius)) {
        // Re-run the init sequence next cycle: it starts with a bus reset and
        // re-applies the resolution, which is what a sensor that browned out
        // or was reconnected needs. Without this one failed read would silence
        // the device until it was power-cycled.
        Log.warn("DS18B20 read failed; re-initialising on next cycle");
        owRelease();
        sensorOk = false;
        return;
    }

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"temperature\":%.2f}",
             celsius + TEMPERATURE_OFFSET_C);

    if (mqtt.isConnected()) {
        mqtt.publish(stateTopic.c_str(), payload);
        Log.info("published: %s", payload);
    } else {
        Log.info("sensor (mqtt offline): %s", payload);
    }
}
