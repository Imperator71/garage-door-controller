#include <Arduino.h>
#include <ETH.h>
#include <PubSubClient.h>   // Install: "PubSubClient" by Nick O'Leary
//1
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
#define TOPIC_DOOR   "garage/door/state"   // OPEN / CLOSED / UNKNOWN
#define TOPIC_PIR    "garage/pir"          // ON / OFF
#define TOPIC_RADAR  "garage/radar"        // ON / OFF


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
// State cache — empty string means "not yet published"
// Forces publish of every topic on first successful MQTT connect
// ============================================================
String lastDoor  = "";
String lastPir   = "";
String lastRadar = "";


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

  // Ethernet — confirmed via Waveshare's own esp-idf example (05_ethernetbasic/Kconfig.projbuild):
  //   PHY: IP101 (= ETH_PHY_TLK110), Addr:1, MDC:31, MDIO:52, POWER/RST:51
  //   RMII: CLK=50 (EMAC_CLK_EXT_IN), TX_EN=49, TX0=34, TX1=35, RX0=29, RX1_EN=30, CRS_DV=28
  // These match pins_arduino.h for esp32p4 — ETH.begin() with no args is correct for this board.
  WiFi.onEvent(onEthEvent);
  ETH.begin();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
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
  // RELAY_GARAGE is not auto-triggered; will be controlled via MQTT command (Phase 1)

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

  delay(200);
}