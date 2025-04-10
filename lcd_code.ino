#include <ESP32Servo.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include <LiquidCrystal.h>
LiquidCrystal lcd(26, 4, 16, 15, 5, 17);

// WiFi Credentials
char ssid[] = "DevilWireless";
char pass[] = "123ab456cd";

// ThingSpeak MQTT Credentials
#define channelID 2913872
const char mqttUserName[] = "KhoIBTInBCEmOBssKx4GMzU";
const char clientID[]     = "KhoIBTInBCEmOBssKx4GMzU";
const char mqttPass[]     = "GsyH3cfmxgqQsvl7k/7bsEzb";
const char* server = "mqtt3.thingspeak.com";
#define mqttPort 1883

WiFiClient client;
PubSubClient mqttClient(client);

// Update interval for ThingSpeak (in seconds)
const int updateInterval = 15;
unsigned long lastPublishMillis = 0;

// ----- Hardware Pin Definitions -----
#define IR_GATE_1 14
#define IR_GATE_2 33
#define IR_SLOT_1 27
#define IR_SLOT_2 25

#define LED_SLOT_1 32
#define LED_SLOT_2 15

#define SERVO_PIN 13
#define SS_PIN 21
#define RST_PIN 22

MFRC522 rfid(SS_PIN, RST_PIN);
Servo barrier;

// ----- Slot State Tracking -----
bool slotOccupied[2] = {false, false};
bool lastSlotOccupied[2] = {false, false};

// ----- Setup WiFi & MQTT -----
void connectWifi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected.");
}

void mqttConnect() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect(clientID, mqttUserName, mqttPass)) {
      Serial.println(" connected.");
    } else {
      Serial.print(" failed, rc = ");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

void mqttPublish(int s1, int s2) {
  String topic = "channels/" + String(channelID) + "/publish";
  String payload = "field1=" + String(s1) + "&field2=" + String(s2);
  mqttClient.publish(topic.c_str(), payload.c_str());
  Serial.println("Published to ThingSpeak: " + payload);
}

// ----- Setup -----
void setup() {
    lcd.begin(16, 2);
    lcd.print("Smart Parking");
    delay(2000);
  Serial.begin(9600);
  delay(3000);

  connectWifi();
  mqttClient.setServer(server, mqttPort);
  randomSeed(analogRead(0));

  pinMode(IR_GATE_1, INPUT);
  pinMode(IR_GATE_2, INPUT);
  pinMode(IR_SLOT_1, INPUT);
  pinMode(IR_SLOT_2, INPUT);

  pinMode(LED_SLOT_1, OUTPUT);
  pinMode(LED_SLOT_2, OUTPUT);

  barrier.attach(SERVO_PIN);
  barrier.write(0);  // Initial gate position: closed

  SPI.begin();
  rfid.PCD_Init();

  Serial.println("System Ready (IR + RFID + MQTT)");
}

// ----- Main Loop -----
void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (!mqttClient.connected()) mqttConnect();
  mqttClient.loop();

  updateSlotStatus();

  // ENTRY FLOW
  if (digitalRead(IR_GATE_1) == LOW) {
    Serial.println("ENTRY: IR1 Triggered");
    if (scanRFID("ENTRY")) {
      openGate();  // Open immediately after RFID
      unsigned long entryTime = millis();
      while (millis() - entryTime < 5000) {
        if (digitalRead(IR_GATE_2) == LOW) {
          int availableSlots[2], count = 0;
          for (int i = 0; i < 2; i++) {
            if (!slotOccupied[i]) availableSlots[count++] = i;
          }
          if (count > 0) {
            int chosen = availableSlots[random(0, count)];
            slotOccupied[chosen] = true;
            Serial.print("ENTRY CONFIRMED → Slot ");
            Serial.print(chosen + 1);
            Serial.println(" assigned.");
          } else {
            Serial.println("ENTRY DENIED → No slot available");
          }
          break;
        }
      }
    }
    delay(1000);  // Cooldown
  }

  // EXIT FLOW
  if (digitalRead(IR_GATE_2) == LOW) {
    Serial.println("EXIT: IR2 Triggered");
    if (scanRFID("EXIT")) {
      openGate();  // Open immediately after RFID
      unsigned long exitTime = millis();
      while (millis() - exitTime < 5000) {
        if (digitalRead(IR_GATE_1) == LOW) {
          Serial.println("EXIT CONFIRMED → Freeing one slot");
          releaseSlot();
          break;
        }
      }
    }
    delay(1000);  // Cooldown
  }

  // Periodic ThingSpeak Update
  if (millis() - lastPublishMillis > updateInterval * 1000) {
    mqttPublish(slotOccupied[0] ? 1 : 0, slotOccupied[1] ? 1 : 0);
    lastPublishMillis = millis();
  }

  delay(100);
}

// ----- Slot Monitoring -----
void updateSlotStatus() {
  bool currentSlot1 = digitalRead(IR_SLOT_1) == LOW;
  bool currentSlot2 = digitalRead(IR_SLOT_2) == LOW;

  slotOccupied[0] = currentSlot1;
  slotOccupied[1] = currentSlot2;

  digitalWrite(LED_SLOT_1, currentSlot1 ? HIGH : LOW);
  digitalWrite(LED_SLOT_2, currentSlot2 ? HIGH : LOW);

  if (currentSlot1 && !lastSlotOccupied[0])
    Serial.println("Slot 1 is now Occupied");
  if (currentSlot2 && !lastSlotOccupied[1])
    Serial.println("Slot 2 is now Occupied");

  lastSlotOccupied[0] = currentSlot1;
  lastSlotOccupied[1] = currentSlot2;
}

// ----- Gate Control -----
void openGate() {
  barrier.write(90);  // Open
  delay(3000);
  barrier.write(0);   // Close
  Serial.println("Gate closed.");
}

// ----- Release Slot -----
void releaseSlot() {
  for (int i = 0; i < 2; i++) {
    if (slotOccupied[i]) {
      slotOccupied[i] = false;
      Serial.print("Slot ");
      Serial.print(i + 1);
      Serial.println(" freed.");
      return;
    }
  }
}

// ----- RFID Scanning -----
bool compareUID(byte *uid, byte *allowedUID) {
  for (int i = 0; i < 4; i++) {
    if (uid[i] != allowedUID[i]) return false;
  }
  return true;
}

bool scanRFID(String type) {
  Serial.print(type); Serial.println(": Waiting for RFID scan (5s)");

  byte allowedUIDs[3][4] = {
    {0x55, 0x33, 0xE0, 0x00},
    {0x36, 0x3B, 0x0E, 0x4E},
    {0xA6, 0x15, 0x8F, 0x40}
  };

  unsigned long start = millis();
  while (millis() - start < 5000) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      Serial.print("RFID UID ("); Serial.print(type); Serial.print("): ");
      for (byte i = 0; i < rfid.uid.size; i++) {
        Serial.print(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        Serial.print(rfid.uid.uidByte[i], HEX);
        Serial.print(" ");
      }
      Serial.println();

      for (int i = 0; i < 3; i++) {
        if (compareUID(rfid.uid.uidByte, allowedUIDs[i])) {
          rfid.PICC_HaltA();
          Serial.println("Access Granted.");
          return true;
        }
      }

      rfid.PICC_HaltA();
      Serial.println("Access Denied.");
      return false;
    }
  }

  Serial.println("RFID scan timeout.");
  return false;
}


