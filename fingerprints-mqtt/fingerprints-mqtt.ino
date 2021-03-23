#include <FS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>

#include <Adafruit_Fingerprint.h>
#include "arduino_secrets.h"

char mqttHost[32]       = "homeassistant.local";
char mqttPort[6]        = "1883";
char mqttUsername[16]   = "mqtt";
char mqttPassword[16]   = "mqtt";
char gateId[32]         = "main";


#define SSID_FOR_SETUP                "Fingerprint-Setup"
#define HOSTNAME                      "fingerprint-mqtt"

#define STATE_TOPIC                   "/fingerprint/status"
#define MODE_LEARNING                 "/fingerprint/learn"
#define MODE_READING                  "/fingerprint/read"
#define MODE_DELETE                   "/fingerprint/delete"
#define AVAILABILITY_TOPIC            "/fingerprint/available"

#define MQTT_MAX_PACKET_SIZE 256
#define MQTT_INTERVAL 500             

// Sensor is connected to: green D5, yellow D6, 3v3
#define SENSOR_TX 12
#define SENSOR_RX 14

SoftwareSerial mySerial(SENSOR_TX, SENSOR_RX);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

WiFiClient wifiClient;
PubSubClient client(wifiClient);

uint8_t id = 0;
uint8_t lastId = 0;

uint8_t lastConfidenceScore = 0;
String sensorMode = "reading";
String lastSensorMode = "";
String sensorState = "";
String lastSensorState = "";

bool shouldSaveConfig = false;
unsigned long lastMQTTmsg = 0;


DynamicJsonDocument mqttMessage(MQTT_MAX_PACKET_SIZE);
char mqttBuffer[MQTT_MAX_PACKET_SIZE];

void setup() {

  setupDevices();
  readConfig();
  setupWifi();
  saveConfig();
  setupMqtt();
 
  ledReady();
}

void saveConfig() {

  if(!shouldSaveConfig) {
    
    return;
  }
  
}

void readConfig() {

  if (SPIFFS.begin()) {
    
    Serial.println("Mounted file system");
    
    if (SPIFFS.exists("/config.json")) {
      
      Serial.println("Reading config file");
      
      File configFile = SPIFFS.open("/config.json", "r");
      
      if (configFile) {
        
        Serial.println("Opened config file");

        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, configFile);
        
        if (error) {
        
          Serial.println(F("Failed to read file, using default configuration"));

        } else {

          Serial.println("Loaded config:");
          serializeJson(doc, Serial);

          strcpy(mqttHost,      doc["mqttHost"]);
          strcpy(mqttPort,      doc["mqttPort"]);
          strcpy(mqttUsername,  doc["mqttUsername"]);
          strcpy(mqttPassword,  doc["mqttPassword"]);
          strcpy(gateId,        doc["gateId"]);
        }
      }
      
      configFile.close();
    }
    
  } else {
    
    Serial.println("Failed to mount FS");
  }
}


void saveConfigCallback () {

  Serial.println("Setup complete.");
  shouldSaveConfig = true;
}

void setupMqtt() {

  client.setServer(mqttHost, atoi(mqttPort));
  client.setCallback(callback);

  mqttMessage["gate"]       = gateId;
  mqttMessage["mode"]       = "reading";
  mqttMessage["match"]      = false;
  mqttMessage["state"]      = "Not matched";
  mqttMessage["id"]         = 0;
  mqttMessage["user"]       = 0;
  mqttMessage["confidence"] = 0;

  while (!client.connected()) {

    Serial.print("Connecting to MQTT...");

    if (client.connect(HOSTNAME, mqttUsername, mqttPassword, AVAILABILITY_TOPIC, 1, true, "offline")) {
      
      Serial.println("connected");

      client.publish(AVAILABILITY_TOPIC, "online");
      client.subscribe(MODE_LEARNING);
      client.subscribe(MODE_READING);
      client.subscribe(MODE_DELETE);

    } else {

      Serial.print("failed, rc: "); Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      
      delay(5000);
    }
  }
}

void setupDevices() {
  
  Serial.begin(57600);
  
  while (!Serial); delay(100);
  
  Serial.println("\n\nWelcome to Fingerprint-MQTT sensor");

  finger.begin(57600); delay(5);

  Serial.print("Looking for sensor...");
  
  if (finger.verifyPassword()) {

    Serial.println("ok");

  } else {

    Serial.println("ko.\nSensor not found: check serial connection on green/yellow cables.");

    while (1) { delay(10);}
  }
}

void setupWifi() {

  Serial.println("Configuration check...");
  
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  wifiManager.autoConnect(SSID_FOR_SETUP);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

 
/*
 WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting...");

  while (WiFi.status() != WL_CONNECTED) {       // Wait till Wifi connected

    delay(500);
    Serial.print(".");
  }

  Serial.println();

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());                     // Print IP address
 */
}

void loop() {

  setupMqtt();
  
  if (sensorMode == "reading") {

    mqttMessage["mode"] = "reading";

    uint8_t result = getFingerprintId();

    if (result == FINGERPRINT_OK) {

      mqttMessage["match"] = true;
      mqttMessage["state"] = "Matched";
      mqttMessage["id"] = lastId;
      mqttMessage["confidence"] = lastConfidenceScore;
      publish();

      lastMQTTmsg = millis();
      delay(200);

    } else if (result == FINGERPRINT_NOTFOUND) {

      mqttMessage["match"] = false;
      mqttMessage["id"] = id;
      mqttMessage["state"] = "Not matched";
      mqttMessage["confidence"] = lastConfidenceScore;
      publish();

      lastMQTTmsg = millis();
      delay(100);

    } else if (result == FINGERPRINT_NOFINGER) {

      if ((millis() - lastMQTTmsg) > MQTT_INTERVAL) {

        mqttMessage["match"] = false;
        mqttMessage["id"] = id;
        mqttMessage["state"] = "Waiting";
        mqttMessage["confidence"] = 0;
        publish();

        lastMQTTmsg = millis();
      }

      if ((millis() - lastMQTTmsg) < 0) {

        lastMQTTmsg = millis();     //Just in case millis ever rolls over
      }
    }
  }

  client.loop();
  delay(100);            //don't need to run this at full speed.
}

uint8_t getFingerprintId() {

  uint8_t p = finger.getImage();

  switch (p) {

    case FINGERPRINT_OK:

      Serial.println("Image taken");
      break;

    case FINGERPRINT_NOFINGER:

      //Serial.println("No finger detected");
      return p;

    case FINGERPRINT_PACKETRECIEVEERR:

      Serial.println("Communication error");
      return p;

    case FINGERPRINT_IMAGEFAIL:

      Serial.println("Imaging error");
      return p;

    default:
      Serial.println("Unknown error");
      return p;
  }

  // OK success!

  p = finger.image2Tz();

  switch (p) {

    case FINGERPRINT_OK:

      Serial.println("Image converted");
      break;

    case FINGERPRINT_IMAGEMESS:

      Serial.println("Image too messy");
      return p;

    case FINGERPRINT_PACKETRECIEVEERR:

      Serial.println("Communication error");
      return p;

    case FINGERPRINT_FEATUREFAIL:

      Serial.println("Could not find fingerprint features");
      return p;

    case FINGERPRINT_INVALIDIMAGE:

      Serial.println("Could not find fingerprint features");
      return p;

    default:

      Serial.println("Unknown error");
      return p;
  }

  // OK converted!
  p = finger.fingerSearch();

  if (p == FINGERPRINT_OK) {

    Serial.println("Found a print match!");

    lastId = finger.fingerID;
    lastConfidenceScore = finger.confidence;
    ledMatch();
    return p;

  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {

    Serial.println("Communication error");
    return p;

  } else if (p == FINGERPRINT_NOTFOUND) {

    Serial.println("Did not find a match");
    lastConfidenceScore = finger.confidence;
    ledWrong();
    return p;

  } else {

    Serial.println("Unknown error");
    return p;
  }
}

uint8_t getFingerprintEnroll() {

  int p = -1;

  sensorMode = "learning";
  mqttMessage["mode"] = "learning";
  mqttMessage["id"] = id;
  mqttMessage["state"] = "Place finger...";
  mqttMessage["confidence"] = 0;
  publish();

  Serial.print("Waiting for valid finger to enroll as #"); Serial.println(id);

  while (p != FINGERPRINT_OK) {

    p = finger.getImage();

    switch (p) {

      case FINGERPRINT_OK:

        Serial.println("Image taken");
        ledFinger();
        break;

      case FINGERPRINT_NOFINGER:

        Serial.print(".");
        ledWait();
        break;

      case FINGERPRINT_PACKETRECIEVEERR:

        Serial.println("Communication error");
        break;

      case FINGERPRINT_IMAGEFAIL:

        Serial.println("Imaging error");
        break;

      default:

        Serial.println("Unknown error");
        break;
    }
  }

  // OK success!

  p = finger.image2Tz(1);

  switch (p) {

    case FINGERPRINT_OK:

      Serial.println("Image converted");
      break;

    case FINGERPRINT_IMAGEMESS:

      Serial.println("Image too messy");
      return p;

    case FINGERPRINT_PACKETRECIEVEERR:

      Serial.println("Communication error");
      return p;

    case FINGERPRINT_FEATUREFAIL:

      Serial.println("Could not find fingerprint features");
      return p;

    case FINGERPRINT_INVALIDIMAGE:

      Serial.println("Could not find fingerprint features");
      return p;

    default:
      Serial.println("Unknown error");
      return p;
  }

  sensorMode = "learning";
  mqttMessage["mode"] = "learning";
  mqttMessage["id"] = id;
  mqttMessage["state"] = "Remove finger...";
  mqttMessage["confidence"] = 0;
  publish();

  Serial.println("Remove finger");
  delay(2000);

  p = 0;

  while (p != FINGERPRINT_NOFINGER) {

    p = finger.getImage();
  }

  Serial.print("Id "); Serial.println(id);

  p = -1;

  sensorMode = "learning";
  mqttMessage["mode"] = "learning";
  mqttMessage["id"] = id;
  mqttMessage["state"] = "Place same finger again...";
  mqttMessage["confidence"] = 0;
  publish();

  Serial.println("Place same finger again");

  while (p != FINGERPRINT_OK) {

    p = finger.getImage();

    switch (p) {

      case FINGERPRINT_OK:

        Serial.println("Image taken");
        ledFinger();
        break;

      case FINGERPRINT_NOFINGER:

        Serial.print(".");
        ledWait();
        break;

      case FINGERPRINT_PACKETRECIEVEERR:

        Serial.println("Communication error");
        break;

      case FINGERPRINT_IMAGEFAIL:

        Serial.println("Imaging error");
        break;

      default:

        Serial.println("Unknown error");
        break;
    }
  }

  // OK success!

  p = finger.image2Tz(2);

  switch (p) {

    case FINGERPRINT_OK:

      Serial.println("Image converted");
      break;
      
    case FINGERPRINT_IMAGEMESS:

      Serial.println("Image too messy");
      return p;

    case FINGERPRINT_PACKETRECIEVEERR:

      Serial.println("Communication error");
      return p;

    case FINGERPRINT_FEATUREFAIL:

      Serial.println("Could not find fingerprint features");
      return p;

    case FINGERPRINT_INVALIDIMAGE:

      Serial.println("Could not find fingerprint features");
      return p;

    default:

      Serial.println("Unknown error");
      return p;
  }

  // OK converted!
  Serial.print("Creating model for #");  Serial.println(id);

  p = finger.createModel();

  if (p == FINGERPRINT_OK) {

    Serial.println("Prints matched!");

  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {

    Serial.println("Communication error");
    return p;

  } else if (p == FINGERPRINT_ENROLLMISMATCH) {

    Serial.println("Fingerprints did not match");
    return p;

  } else {

    Serial.println("Unknown error");
    return p;
  }

  Serial.print("Id "); Serial.println(id);

  p = finger.storeModel(id);

  if (p == FINGERPRINT_OK) {

    sensorMode = "learning";
    mqttMessage["mode"] = "learning";
    mqttMessage["id"] = id;
    mqttMessage["state"] = "Success, stored";
    mqttMessage["confidence"] = 0;
    publish();

    Serial.println("Stored!");
    return true;

  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {

    Serial.println("Communication error");
    return p;

  } else if (p == FINGERPRINT_BADLOCATION) {

    Serial.println("Could not store in that location");
    return p;

  } else if (p == FINGERPRINT_FLASHERR) {

    Serial.println("Error writing to flash");
    return p;

  } else {

    Serial.println("Unknown error");
    return p;
  }
}

uint8_t deleteFingerprint() {

  uint8_t p = -1;

  p = finger.deleteModel(id);

  if (p == FINGERPRINT_OK) {

    sensorMode = "deleting";
    mqttMessage["mode"] = "deleting";
    mqttMessage["id"] = id;
    mqttMessage["state"] = "Deleted";
    mqttMessage["confidence"] = 0;
    publish();

    ledWrong();
    
    return true;

  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {

    Serial.println("Communication error");
    return p;

  } else if (p == FINGERPRINT_BADLOCATION) {

    Serial.println("Could not delete in that location");
    return p;

  } else if (p == FINGERPRINT_FLASHERR) {

    Serial.println("Error writing to flash");
    return p;

  } else {

    Serial.print("Unknown error: 0x"); Serial.println(p, HEX);
    return p;
  }
}


void publish() {

  const char* state = mqttMessage["state"];
  sensorState = String(state);

  int id = mqttMessage["id"];
  mqttMessage["user"] = id/10;
  
  if ((sensorMode != lastSensorMode) || (sensorState != lastSensorState)) {

    Serial.println("Publishing state... mode: " + sensorMode + " state: " + sensorState);

    lastSensorMode = sensorMode;
    lastSensorState = sensorState;

    size_t mqttMessageSize = serializeJson(mqttMessage, mqttBuffer);
    client.publish(STATE_TOPIC, mqttBuffer, mqttMessageSize);
    Serial.println(mqttBuffer);
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  
  if (strcmp(topic, MODE_LEARNING) == 0) {

    char charArray[3];

    for (int i = 0; i < length; i++) {

      charArray[i] = payload[i];
    }

    id = atoi(charArray);

    if (id > 0 && id < 128) {

      Serial.println("Entering Learning mode");

      sensorMode = "learning";
      mqttMessage["mode"] = "learning";
      mqttMessage["id"] = id;
      mqttMessage["confidence"] = 0;
      publish();

      while (!getFingerprintEnroll());

      Serial.println("Exiting Learning mode");

      sensorMode = "reading";
      mqttMessage["mode"] = "reading";

      id = 0;

    } else {

      Serial.println("Invalid Id");
    }

    Serial.println();
  }

  if (strcmp(topic, MODE_DELETE) == 0) {

    char charArray[3];

    for (int i = 0; i < length; i++) {

      charArray[i] = payload[i];
    }

    id = atoi(charArray);

    if (id > 0 && id < 128) {

      sensorMode = "deleting";
      mqttMessage["mode"] = "deleting";
      mqttMessage["id"] = id;
      mqttMessage["confidence"] = 0;
      publish();

      Serial.println("Entering delete mode");

      while (!deleteFingerprint());

      Serial.println("Exiting delete mode");

      delay(2000);

      sensorMode = "reading";
      mqttMessage["mode"] = "reading";

      id = 0;
    }

    Serial.println();
  }
}

// mode(1-6),delay(1-255),color(1-Red/2-Blue/3-Purple),times(1-255)

void ledFinger() {

  finger.led_control(1, 100, 2, 1);
}

void ledMatch() {

  finger.led_control(1, 150, 2, 1);
}

void ledWrong() {

  finger.led_control(1, 30, 1, 2);
}

void ledReady() {

  finger.led_control(2, 150, 2, 1);
}

void ledWait() {

  finger.led_control(1, 15, 3, 1);
}
