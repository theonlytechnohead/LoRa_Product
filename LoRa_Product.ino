#include <SPI.h>
#include <LoRa.h>
#include "sha.h"

#include <ArduinoJson.h>

#include <ZSharpIR.h>

#include <Update.h>
#include <WiFi.h>
#include <WebServer.h>

#include "SSD1306.h"
#include "images.h"

#define SCK     5    // GPIO5  -- SX1278's SCK
#define MISO    19   // GPIO19 -- SX1278's MISO
#define MOSI    27   // GPIO27 -- SX1278's MOSI
#define SS      18   // GPIO18 -- SX1278's CS
#define RST     14   // GPIO14 -- SX1278's RESET
#define DI0     26   // GPIO26 -- SX1278's IRQ(Interrupt Request)

#define BAND    915E6 // LoRa band frequency, XXXE6, where XXX is MHz
#define SYNC_WORD 232 //HEX: 0xE8 // ranges from 0x00-0xFF, default 0x12, see API docs


// Constants
String ssid = "KeepBox ";
String password;

// Webserver
WebServer server(80);
const char* uploadPage = "<form method='POST' action='/update' enctype='multipart/form-data'> <input type='file' name='update'> <input type='submit' value='Update KeepBox'> </form>";

// Constants
const char* getSignalInfoCommand = "GET_SIGNAL_INFO";
const char* updateReadyCommand = "READY_UPDATE";

// Variables
bool wifi = false;
bool lora = true;
unsigned int lastMillis = 0;
unsigned int interval = 4000;

SSD1306 display(0x3c, 4, 15);
ZSharpIR SharpIR(34, 1080);
TaskHandle_t IRtask;

String rssi = "--";
String snr = "--";
String packSize = "--";


// Multitasking function
void handleIRtask (void* parameter) {
  for (;;) {
    //Serial.println(SharpIR.distance());
    Serial.println(analogRead(34));
    delay(250);
  }
}

void displayLoRaData (String dataToDisplay) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  display.drawString(0, 0, "RSSI: " + rssi);
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 0, snr + " :SNR");
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  if (wifi) {
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 12, ssid);
    display.drawString(64, 22, password);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawStringMaxWidth(0, 35, 128, dataToDisplay.c_str());
  } else {
    display.drawString(0, 10, "Got: " + packSize + " bytes");
    display.drawStringMaxWidth(0, 26, 128, dataToDisplay.c_str());
  }

  display.display();
}

void updateDisplay (String dataReceived) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  display.drawString(0, 0, "Update received");
  display.drawString(0, 10, "Received " + packSize + " bytes");
  display.drawString(0, 26, dataReceived);
  display.display();
}

void sendLoraJson (DynamicJsonDocument doc) {
  LoRa.idle();
  digitalWrite(2, HIGH);

  char output[measureJson(doc) + 1];
  serializeJson(doc, output, sizeof(output));
  //Serial.println(output);

  // send packet
  LoRa.beginPacket();
  LoRa.print(output);
  LoRa.endPacket();

  lastMillis = millis();
  digitalWrite(2, LOW);

  LoRa.receive();
}

void sendSignalInfo () {
  //DynamicJsonDocument doc(JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(1)); // Actually thats still too small!!?!?!?!?!
  DynamicJsonDocument doc(2048);

  String payload = "RSSI: " + rssi + "\nSNR: " + snr;
  
  String checksum = SHA256(payload);

  doc["checksum"] = checksum;
  doc["command"] = getSignalInfoCommand;
  doc["payload"] = payload;

  sendLoraJson(doc);
}

void startWiFi () {
  wifi = true;
  for (int i = 0; i < 3; i++) {
    ssid += String(random(10));
  }
  for (int i = 0; i < 8; i++) {
    password += String(random(10));
  }
  WiFi.softAPConfig(IPAddress(10, 0, 0, 1), IPAddress(10, 0, 0, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid.c_str(), password.c_str());

  server.on("/upload", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", uploadPage);
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "Something went wrong" : "All done!");
    ESP.restart();
  }, []() {
    lora = false;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!Update.begin()) {
        //start with max available size
        Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        //Update.printError(Serial);
      }
    }
    else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        //true to set the size to the current progress
      } else {
        Update.printError(Serial);
      }
    } else {
      //Serial.printf("Update Failed Unexpectedly (likely broken connection): status=%d\n", upload.status);
    }
  });

  server.begin();
}

void parseData(int packetSize) {
  String packet;
  packSize = String(packetSize, DEC);
  for (int i = 0; i < packetSize; i++) {
    packet += (char) LoRa.read();
  }
  rssi = String(LoRa.packetRssi(), DEC);
  snr = String(LoRa.packetSnr(), 1);
  lastMillis = millis();

  DynamicJsonDocument doc(2048);

  deserializeJson(doc, packet);

  String checksum = doc["checksum"];
  auto command = doc["command"];
  String payload = doc["payload"];

  String calc = SHA256(payload);

  delay(500); // Just give it a moment to calm down a bit

  if (checksum == calc) {
    if (command == updateReadyCommand) {
      startWiFi();
      sendSignalInfo();
      displayLoRaData(command);
    } else if (command == getSignalInfoCommand) {
      sendSignalInfo();
      displayLoRaData(payload);
    }
  } else {
    sendSignalInfo();
    //displayLoRaData("Invalid checksum!");
    displayLoRaData(checksum);
  }
}

void setup() {
  pinMode(36, INPUT);
  pinMode(2, OUTPUT);
  pinMode(16, OUTPUT);
  digitalWrite(16, LOW);    // set GPIO16 low to reset OLED
  delay(50);
  digitalWrite(16, HIGH); // while OLED is running, must set GPIO16 in high、

  if (wifi) {
    startWiFi();
  }

  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DI0);
  if (!LoRa.begin(915E6)) {
    //display.drawString(0, 0, "Starting LoRa failed!");
    while (1);
  }

  LoRa.sleep();
  LoRa.setSyncWord(SYNC_WORD);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(10);
  LoRa.setCodingRate4(8);
  LoRa.setSignalBandwidth(250E3);
  LoRa.idle();

  //LoRa.onReceive(cbk);
  LoRa.receive();

  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  xTaskCreatePinnedToCore(
      handleIRtask, // Function to implement the task
      "IRtask", // Name of the task
      8192,  // Stack size in words (causes stack overflow (DUH!!!!) if too low
      NULL,  // Task input parameter
      0,  // Priority of the task, 0 is lowest
      &IRtask,  // Task handle
      0); // Core where the task should run, code runs on core 1 by defaul

  //SPIFFS.begin();
  Serial.begin(115200);

  delay(1500);
  displayLoRaData("");
}
/*
uint16_t get_gp2d12 (uint16_t value) {
    if (value < 10) value = 10;
    return ((67870.0 / (value - 3.0)) - 40.0);
}*/

int read_distance () {
  float volts = analogRead(36)*0.00122070312;  // value from sensor * (5/4096)
  int distance = 13*pow(volts, -1);
  if (distance > 800) distance = 800;
  return distance;
}

void loop() {
  //Web stuff
  if (wifi) {
    server.handleClient();
  }

  if (lora) {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      parseData(packetSize);
    }

    if (millis() - lastMillis > interval) {
      rssi = "--";
      snr = "--";
      packSize = "--";
      displayLoRaData("No data received");
    }
  }
}
