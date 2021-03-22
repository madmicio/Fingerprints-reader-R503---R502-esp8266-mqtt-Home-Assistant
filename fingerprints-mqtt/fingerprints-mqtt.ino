/*************************************
   https://everythingsmarthome.co.uk

   This is an MQTT connected fingerprint sensor which can
   used to connect to your home automation software of choice.

   You can add and remove fingerprints using MQTT topics by
   sending the Id through the topic.

   Simply configure the Wifi and MQTT parameters below to get
   started!

*/

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Adafruit_Fingerprint.h>
#include "arduino_secrets.h"

// NB: put secret values in a file named arduino_secrets.h in the same folder
// that file is NOT shared via git and must be created locally
// Eg. #define SECRET_WIFI_SSID "YourWiFiName"

char WIFI_SSID[]      = SECRET_WIFI_SSID;
char WIFI_PASSWORD[]  = SECRET_WIFI_PASSWORD;
char MQTT_USERNAME[]  = SECRET_MQTT_USERNAME;
char MQTT_PASSWORD[]  = SECRET_MQTT_PASSWORD;

// MQTT Settings
#define HOSTNAME                      "fingerprint-main"
#define MQTT_SERVER                   "192.168.1.12"
#define GATE_ID                       "main"
#define STATE_TOPIC                   "/fingerprint/status"
#define MODE_LEARNING                 "/fingerprint/learn"
#define MODE_READING                  "/fingerprint/read"
#define MODE_DELETE                   "/fingerprint/delete"
#define AVAILABILITY_TOPIC            "/fingerprint/available"

#define MQTT_MAX_PACKET_SIZE 256
#define MQTT_INTERVAL 500             

#define SENSOR_TX 12                  //GPIO Pin for RX
#define SENSOR_RX 14                  //GPIO Pin for TX

SoftwareSerial mySerial(SENSOR_TX, SENSOR_RX);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

WiFiClient wifiClient;                // Initiate WiFi library
PubSubClient client(wifiClient);      // Initiate PubSubClient library

uint8_t id = 0;                       //Stores the current fingerprint Id
uint8_t lastId = 0;                   //Stores the last matched Id
uint8_t lastConfidenceScore = 0;      //Stores the last matched confidence score
String sensorMode = "reading";        //Current sensor mode
String lastSensorMode = "";
String lastSensorState = "";
String currentState = "";
unsigned long lastMQTTmsg = 0;        //Stores millis since last MQTT message

//Declare JSON variables
DynamicJsonDocument mqttMessage(MQTT_MAX_PACKET_SIZE);
char mqttBuffer[MQTT_MAX_PACKET_SIZE];

void setup() {

  Serial.begin(57600);

  while (!Serial);

  delay(100);
  Serial.println("\n\nWelcome to the MQTT Fingerprint Sensor program!");

  // set the data rate for the sensor serial port
  finger.begin(57600);

  delay(5);

  if (finger.verifyPassword()) {

    Serial.println("Found fingerprint sensor!");

  } else {

    Serial.println("Did not find fingerprint sensor :(");

    while (1) {
      delay(1);
    }
  }

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

  client.setServer(MQTT_SERVER, 1883);                // Set MQTT server and port number
  client.setCallback(callback);

  // Init mqtt payload
  mqttMessage["gate"] = GATE_ID;
  mqttMessage["mode"] = "reading";
  mqttMessage["match"] = false;
  mqttMessage["state"] = "Not matched";
  mqttMessage["id"] = 0;
  mqttMessage["user"] = 0;
  mqttMessage["confidence"] = 0;

  ledReady();
}

void loop() {

  if (!client.connected()) {

    reconnect();                //Just incase we get disconnected from MQTT server
  }

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

void reconnect() {

  while (!client.connected()) {

    Serial.print("Attempting MQTT connection...");

    if (client.connect(HOSTNAME, MQTT_USERNAME, MQTT_PASSWORD, AVAILABILITY_TOPIC, 1, true, "offline")) {
      
      Serial.println("connected");

      client.publish(AVAILABILITY_TOPIC, "online");
      client.subscribe(MODE_LEARNING);
      client.subscribe(MODE_READING);
      client.subscribe(MODE_DELETE);

    } else {

      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      
      delay(5000);
    }
  }
}

void publish() {

  const char* state = mqttMessage["state"];
  currentState = String(state);

  int id = mqttMessage["id"];
  mqttMessage["user"] = id/10;
  
  if ((sensorMode != lastSensorMode) || (currentState != lastSensorState)) {

    Serial.println("Publishing state... mode: " + sensorMode + " state: " + currentState);

    lastSensorMode = sensorMode;
    lastSensorState = currentState;

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
