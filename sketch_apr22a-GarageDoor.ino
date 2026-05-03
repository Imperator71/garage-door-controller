#include <Arduino.h>
#include <ETH.h>
#include <PubSubClient.h>           // Install: "PubSubClient" by Nick O'Leary
#include <Adafruit_Fingerprint.h>   // Install: "Adafruit Fingerprint Sensor Library"

/*
Garage Door Opening, Door Sensing and Fingerprint Sensor board
---
Phase1:
The Board is Supposed to be connected via POE LAN, It should report The Garage Status via MQTT to Homeassistant.
It should steer the garage door via Homeassistant (MQTT?) or via Fingerprint.

Phase2:
It should be able to sync the fingerprint with other sensors.
It should be able to act as a Fingerprint checkin if needed (MQTT Message with timeout?)

Phase3:
It should be able to sense presence inside the garage and optionally switch an LED via the dual relay


Controllerboard:
ESP32 LAN POE: Waveshare ESP32-P4-POE-ETH-NH-KIT-A | Connected in Production via POE Lan | [Avoid Dualuse Pins GPIO20 GPIO21 GPIO36 GPIO24 GPIO25 and Straping Pins GPIO34–GPIO38 and Boot mdoe Pins GPIO35–GPIO38]
Fingerprint: Capacitive Fingerprint Sensor R503 | UART 57600bps
Close/Open Sensing (*2): DE332 Magnet Switch Normally closed: close magnet - connect circuit, magnet removed- break circuit Working current: 0.5 A / Working voltage: 100 V (1-100 V) / Maximum power: 10 W
Steering: 2* 3V Relay AYWHP Relay Board Optocoupler Optoisolation High Level Trigger for IOT AC 250V 10A; DC 30V 10A PINS [VCC IN GND and NC COM NO]

PIR Motion Sensor: HAILANGNIAO AM312 Mini Pyroelectric PIR | 3.3V logic | Digital OUT HIGH on motion | ~2m range | GPIO4
Distance Sensor (not Implemented, ignore for now): AZDelivery VL53L0X Time-of-Flight (ToF) Laser Ranging Sensor
Presence Sensor: Waveshare Human Micro-Motion Detection MmWave Sensor, 24 GHz | Digital OUT HIGH on presence | GPIO5

Existing Hardware:
Garage door: Marantec Comfort 220 Garageopener, Shorting 2 Wires (via the relay) will active the door (Open Stop Close)
Simple LED Strip 5v (via relay)

*/


// ============================================================
// Configuration — adjust to your environment
// ============================================================
#define MQTT_SERVER  "192.168.1.1"    // ← Change to your broker IP
#define MQTT_PORT    1883
//#define MQTT_USER  "username"       // ← Uncomment if broker requires auth
//#define MQTT_PASS  "password"

// MQTT topics (retained, so Home Assistant gets state after any restart)
#define TOPIC_DOOR        "garage/door/state"       // OPEN / CLOSED / UNKNOWN
#define TOPIC_PIR         "garage/pir"              // ON / OFF
#define TOPIC_RADAR       "garage/radar"            // ON / OFF
#define TOPIC_FINGERPRINT "garage/fingerprint"      // GRANTED:<id> / DENIED / UNAVAILABLE
#define TOPIC_FP_CMD      "garage/fingerprint/cmd"  // inbound: ENROLL:<id> | DELETE:<id>
#define TOPIC_FP_RESULT   "garage/fingerprint/result" // outbound: OK:<id> | FAIL | DELETED:<id> | BUSY | ENROLL_TIMEOUT | ENROLL_MISMATCH
#define TOPIC_FP_SLOTS    "garage/fingerprint/slots"  // outbound: comma-separated occupied slot IDs


// ============================================================
// Pin Definitions
// ============================================================
const int RELAY_GARAGE  = 26;
const int RELAY_LED     = 27;

const int SENSOR_CLOSED = 32;  // DE332 NC switch — LOW when magnet present (door closed)
const int SENSOR_OPEN   = 33;  // DE332 NC switch — LOW when magnet present (door open)

const int PIR_MOTION    = 4;   // AM312 — HIGH on motion detected
const int RADAR_OUT     = 5;   // Waveshare MmWave 24 GHz — HIGH when presence detected
                                // ← Wire the sensor's OUT pin here; change if needed

// R503 Fingerprint Sensor (UART1, 57600bps)
// Wiring: sensor VCC→3.3V, GND→GND, TXD→GPIO7(RX), RXD→GPIO6(TX), WAKEUP/TOUCH→GPIO8
const int FP_TX_PIN     = 6;   // ESP TX → sensor RXD
const int FP_RX_PIN     = 7;   // ESP RX ← sensor TXD
const int FP_WAKEUP_PIN = 8;   // sensor TOUCH/WAKEUP out — HIGH when finger present

// Pulse duration for door relay trigger on fingerprint match
const unsigned long RELAY_PULSE_MS = 500;

const bool RELAY_ON  = HIGH;   // Change to LOW if your relay board is active-LOW
const bool RELAY_OFF = LOW;


// ============================================================
// Networking / MQTT
// ============================================================
WiFiClient   netClient;
PubSubClient mqtt(netClient);

bool          ethConnected    = false;
unsigned long lastReconnectMs = 0;

// ============================================================
// Fingerprint sensor
// ============================================================
HardwareSerial      FPSerial(1);
Adafruit_Fingerprint finger(&FPSerial);
bool                 fpReady          = false;
bool                 fpWaitingRelease = false;  // debounce — ignore until finger lifted

// Enrollment state machine
enum FpEnrollState { FP_ENROLL_IDLE, FP_ENROLL_WAIT1, FP_ENROLL_WAIT_RELEASE, FP_ENROLL_WAIT2 };
FpEnrollState fpEnrollState   = FP_ENROLL_IDLE;
uint16_t      fpEnrollId      = 0;
unsigned long fpEnrollStartMs = 0;
#define FP_ENROLL_TIMEOUT_MS 30000UL

// ============================================================
// State cache — empty string means "not yet published"
// Forces publish of every topic on first successful MQTT connect
// ============================================================
String lastDoor  = "";
String lastPir   = "";
String lastRadar = "";


// ============================================================
// MQTT inbound command handler
// Handles: ENROLL:<id>  — start enrollment flow for slot <id>
//          DELETE:<id>  — delete slot <id> from sensor
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) != TOPIC_FP_CMD) return;

  if (msg.startsWith("ENROLL:")) {
    if (!fpReady) { mqtt.publish(TOPIC_FP_RESULT, "SENSOR_UNAVAILABLE", false); return; }
    if (fpEnrollState != FP_ENROLL_IDLE) { mqtt.publish(TOPIC_FP_RESULT, "BUSY", false); return; }
    fpEnrollId      = (uint16_t)msg.substring(7).toInt();
    fpEnrollState   = FP_ENROLL_WAIT1;
    fpEnrollStartMs = millis();
    finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 100, FINGERPRINT_LED_BLUE);
    char res[32];
    snprintf(res, sizeof(res), "ENROLLING:%u", fpEnrollId);
    mqtt.publish(TOPIC_FP_RESULT, res, false);
    Serial.printf("[FP] Enroll requested for slot %u\n", fpEnrollId);
  }
  else if (msg.startsWith("DELETE:")) {
    if (!fpReady) { mqtt.publish(TOPIC_FP_RESULT, "SENSOR_UNAVAILABLE", false); return; }
    uint16_t id = (uint16_t)msg.substring(7).toInt();
    uint8_t  p  = finger.deleteModel(id);
    char res[32];
    if (p == FINGERPRINT_OK) snprintf(res, sizeof(res), "DELETED:%u", id);
    else                     snprintf(res, sizeof(res), "DELETE_FAIL:%u", id);
    mqtt.publish(TOPIC_FP_RESULT, res, false);
    Serial.printf("[FP] Delete slot %u: %s\n", id, p == FINGERPRINT_OK ? "OK" : "FAIL");
  }
}


// ============================================================
// Publish comma-separated list of occupied sensor slot IDs.
// Called once after MQTT connect so the server knows what's
// enrolled on the physical sensor.
// ============================================================
void publishSlotList() {
  if (!fpReady || !mqtt.connected()) return;
  finger.getParameters();
  String slots = "";
  for (uint16_t id = 0; id < finger.capacity && id < 200; id++) {
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      if (slots.length()) slots += ",";
      slots += String(id);
    }
    delay(5);
  }
  mqtt.publish(TOPIC_FP_SLOTS, slots.c_str(), false);
  Serial.printf("[FP] Slot list published: [%s]\n", slots.c_str());
}


// ============================================================
// Ethernet event handler
// ============================================================
void onEthEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("[ETH] IP: %s\n", ETH.localIP().toString().c_str());
      ethConnected = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("[ETH] Disconnected");
      ethConnected = false;
      break;
    default:
      break;
  }
}


// ============================================================
// Non-blocking MQTT reconnect — retries once every 5 s
// Does nothing if Ethernet is down; never blocks loop()
// ============================================================
void maintainMqtt() {
  if (!ethConnected || mqtt.connected()) return;
  if (millis() - lastReconnectMs < 5000) return;
  lastReconnectMs = millis();

  Serial.print("[MQTT] Connecting... ");
#if defined(MQTT_USER)
  bool ok = mqtt.connect("garage-esp32", MQTT_USER, MQTT_PASS);
#else
  bool ok = mqtt.connect("garage-esp32");
#endif
  Serial.println(ok ? "OK" : "failed");

  if (ok) {
    // Clear cache so all topics are re-published on reconnect
    lastDoor = lastPir = lastRadar = "";
    mqtt.subscribe(TOPIC_FP_CMD);
    if (!fpReady) mqtt.publish(TOPIC_FINGERPRINT, "UNAVAILABLE", true);
    publishSlotList();
  }
}


// ============================================================
// Publish a value only when it has changed.
// Retained flag ensures HA keeps the last state after a broker restart.
// ============================================================
void publishIfChanged(const char* topic, const char* value, String& last) {
  if (!mqtt.connected()) return;
  if (String(value) == last) return;
  if (mqtt.publish(topic, value, /*retained=*/true)) {
    last = String(value);
  }
}


// ============================================================
// Non-blocking enrollment state machine — called every loop()
// Drives the two-scan enrollment sequence triggered via MQTT.
// ============================================================
void enrollTick() {
  if (!fpReady || fpEnrollState == FP_ENROLL_IDLE) return;

  if (millis() - fpEnrollStartMs > FP_ENROLL_TIMEOUT_MS) {
    fpEnrollState = FP_ENROLL_IDLE;
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, FINGERPRINT_LED_BLUE);
    mqtt.publish(TOPIC_FP_RESULT, "ENROLL_TIMEOUT", false);
    Serial.println("[FP] Enroll timed out");
    return;
  }

  uint8_t p;
  switch (fpEnrollState) {
    case FP_ENROLL_WAIT1:
      p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) return;
      if (p != FINGERPRINT_OK) return;  // transient, keep waiting
      p = finger.image2Tz(1);
      if (p != FINGERPRINT_OK) {
        // bad image — flash red, keep breathing blue and retry
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_RED, 3);
        delay(400);
        finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 100, FINGERPRINT_LED_BLUE);
        return;
      }
      finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_BLUE, 2);
      Serial.println("[FP] First scan OK — lift finger");
      fpEnrollState = FP_ENROLL_WAIT_RELEASE;
      break;

    case FP_ENROLL_WAIT_RELEASE:
      if (finger.getImage() == FINGERPRINT_NOFINGER) {
        fpEnrollState = FP_ENROLL_WAIT2;
        finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 100, FINGERPRINT_LED_BLUE);
        Serial.println("[FP] Place finger again");
      }
      break;

    case FP_ENROLL_WAIT2:
      p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) return;
      if (p != FINGERPRINT_OK) return;
      p = finger.image2Tz(2);
      if (p != FINGERPRINT_OK) {
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_RED, 3);
        delay(400);
        finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 100, FINGERPRINT_LED_BLUE);
        return;
      }
      p = finger.createModel();
      if (p != FINGERPRINT_OK) {
        finger.LEDcontrol(FINGERPRINT_LED_FLASHING, 25, FINGERPRINT_LED_RED, 6);
        fpEnrollState = FP_ENROLL_IDLE;
        mqtt.publish(TOPIC_FP_RESULT, "ENROLL_MISMATCH", false);
        Serial.println("[FP] Mismatch — fingers didn't match");
        return;
      }
      p = finger.storeModel(fpEnrollId);
      fpEnrollState = FP_ENROLL_IDLE;
      finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, FINGERPRINT_LED_BLUE);
      if (p == FINGERPRINT_OK) {
        char res[32];
        snprintf(res, sizeof(res), "OK:%u", fpEnrollId);
        mqtt.publish(TOPIC_FP_RESULT, res, false);
        Serial.printf("[FP] Enrolled slot %u OK\n", fpEnrollId);
      } else {
        mqtt.publish(TOPIC_FP_RESULT, "ENROLL_FAIL", false);
      }
      break;

    default: break;
  }
}


// ============================================================
// Fingerprint background scan — called every loop()
// Uses WAKEUP pin so we only read the sensor when a finger is present.
// Publishes GRANTED:<id> on match, DENIED on no match.
// On match: pulses the garage relay to trigger the door.
// Safe to call even if sensor is absent (fpReady guards it).
// ============================================================
void fingerprintTick() {
  if (!fpReady) return;
  if (fpEnrollState != FP_ENROLL_IDLE) return;  // enrollment in progress, skip normal scan

  // If we're waiting for the finger to lift, check for release
  if (fpWaitingRelease) {
    if (digitalRead(FP_WAKEUP_PIN) == LOW) fpWaitingRelease = false;
    return;
  }

  // Only proceed when wakeup pin signals a finger is present
  if (digitalRead(FP_WAKEUP_PIN) == LOW) return;

  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.println("[FP] Feature extraction failed");
    if (mqtt.connected()) mqtt.publish(TOPIC_FINGERPRINT, "DENIED", false);
    fpWaitingRelease = true;
    return;
  }

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    char payload[32];
    snprintf(payload, sizeof(payload), "GRANTED:%u", finger.fingerID);
    Serial.printf("[FP] Match! ID=%u confidence=%u\n", finger.fingerID, finger.confidence);
    if (mqtt.connected()) mqtt.publish(TOPIC_FINGERPRINT, payload, false);

    // Pulse garage relay to trigger door
    digitalWrite(RELAY_GARAGE, RELAY_ON);
    delay(RELAY_PULSE_MS);
    digitalWrite(RELAY_GARAGE, RELAY_OFF);
  } else {
    Serial.println("[FP] No match");
    if (mqtt.connected()) mqtt.publish(TOPIC_FINGERPRINT, "DENIED", false);
  }

  fpWaitingRelease = true;
}


// ============================================================
// setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Garage Door Controller ===");

  // Relay outputs — safe OFF state on boot
  pinMode(RELAY_GARAGE, OUTPUT);
  pinMode(RELAY_LED,    OUTPUT);
  digitalWrite(RELAY_GARAGE, RELAY_OFF);
  digitalWrite(RELAY_LED,    RELAY_OFF);

  // Door position sensors (NC switches — pull HIGH when magnet is absent)
  pinMode(SENSOR_CLOSED, INPUT_PULLUP);
  pinMode(SENSOR_OPEN,   INPUT_PULLUP);

  // Presence sensors (have their own pull-down on the module)
  pinMode(PIR_MOTION, INPUT);
  pinMode(RADAR_OUT,  INPUT);

  // R503 Fingerprint sensor
  pinMode(FP_WAKEUP_PIN, INPUT);
  FPSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  fpReady = finger.verifyPassword();
  if (fpReady) {
    Serial.println("[FP] Sensor OK");
    finger.LEDcontrol(FINGERPRINT_LED_BREATHING, 100, FINGERPRINT_LED_BLUE);
    delay(500);
    finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, FINGERPRINT_LED_BLUE);
  } else {
    Serial.println("[FP] Sensor not found — fingerprint disabled (will report via MQTT on connect)");
  }

  // Ethernet — confirmed via Waveshare's own esp-idf example (05_ethernetbasic/Kconfig.projbuild):
  //   PHY: IP101 (= ETH_PHY_TLK110), Addr:1, MDC:31, MDIO:52, POWER/RST:51
  //   RMII: CLK=50 (EMAC_CLK_EXT_IN), TX_EN=49, TX0=34, TX1=35, RX0=29, RX1_EN=30, CRS_DV=28
  // These match pins_arduino.h for esp32p4 — ETH.begin() with no args is correct for this board.
  WiFi.onEvent(onEthEvent);
  ETH.begin();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);   // default 256 is too small for slot list payloads
  mqtt.setKeepAlive(30);
}


// ============================================================
// loop()
// ============================================================
void loop() {
  // Keep MQTT alive — both calls are no-ops when disconnected
  maintainMqtt();
  if (mqtt.connected()) mqtt.loop();

  // ---- Read sensors ----------------------------------------
  bool closedSensor = (digitalRead(SENSOR_CLOSED) == LOW);
  bool openSensor   = (digitalRead(SENSOR_OPEN)   == LOW);
  bool pirActive    = (digitalRead(PIR_MOTION)     == HIGH);
  bool radarActive  = (digitalRead(RADAR_OUT)      == HIGH);

  // ---- Determine door state --------------------------------
  const char* doorState;
  if      (closedSensor && !openSensor)  doorState = "CLOSED";
  else if (openSensor   && !closedSensor) doorState = "OPEN";
  else                                    doorState = "UNKNOWN";

  // ---- LED relay: on when garage is open -------------------
  digitalWrite(RELAY_LED, (strcmp(doorState, "OPEN") == 0) ? RELAY_ON : RELAY_OFF);
  // RELAY_GARAGE is pulsed by fingerprintTick() on a valid match

  // ---- Serial debug ----------------------------------------
  Serial.printf("Door: %-7s | PIR: %-3s | Radar: %-3s | ETH: %-4s | MQTT: %s\n",
    doorState,
    pirActive   ? "ON" : "OFF",
    radarActive ? "ON" : "OFF",
    ethConnected     ? "UP"   : "DOWN",
    mqtt.connected() ? "UP"   : "DOWN");

  // ---- Publish to MQTT (change-only, retained) -------------
  publishIfChanged(TOPIC_DOOR,  doorState,                lastDoor);
  publishIfChanged(TOPIC_PIR,   pirActive   ? "ON":"OFF", lastPir);
  publishIfChanged(TOPIC_RADAR, radarActive ? "ON":"OFF", lastRadar);

  // ---- Fingerprint: enrollment commands + background scan ---
  enrollTick();
  fingerprintTick();

  delay(200);
}