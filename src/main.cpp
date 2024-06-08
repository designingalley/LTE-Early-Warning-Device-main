// Include required libraries
#include "Arduino.h"
#include <ArduinoJson.h>
#include "Audio.h"
#include "SD.h"
#include "FS.h"
#include "HardwareSerial.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "AWS.h"

// WiFi and AWS IoT details
const char *ssid = "Fure";
const char *password = "spaceone";
const char *topic = "AWS/CIER/SUB/2";
const char *pub_topic = "AWS/CIER/INFO/2";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// microSD Card Reader connections
#define I2S_DOUT 26
#define I2S_BCLK 27
#define I2S_LRC 14

// SD CARD SPI pins
#define SD_CS 21
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23

#define EC_RX 16
#define EC_TX 17

#define UserSwitch 22
#define VibraMotor 4
#define OnboardLED 5
#define VP_PIN 36

// Create Audio object
HardwareSerial LTE_Serial(2);
Audio audio;

const String DEVICE_ID = "2";

bool wifiFlag = false;
int FLAG = 0;

unsigned long startTime = 0;
unsigned int duration = 0;
unsigned int mainFlag = 0;
unsigned long flagChangeTime = 0;

unsigned long previousMillis = 0;
const long interval = 1000; // Interval in milliseconds (1 second)

int buttonState = HIGH;     // the current reading from the input pin
int lastButtonState = HIGH; // the previous reading from the input pin
int buttonCount = 0;        // counter for the number of button presses
char findJson[400];

int timeNow = 0;
int Capture = 0;
int warnUser = 0;

int longPress;
int LPD;

float maxBatteryVolatage = 3.7; //act high should be 4.2
float minBatteryVolatage = 3.3;

void receiveATCommand(int);
void playFile(const char *filename, unsigned long duration);
void sendATCommand(String);
void visualIndication();
void playSong(String);

void setupWiFi()
{
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);

  for (int i = 0; i < 20; i++)
  {
    delay(1000);
    Serial.print(".");
    if (WiFi.status() == WL_CONNECTED)
    {
      wifiFlag = true;
      Serial.println("\nConnected to WiFi");
      break;
    }
  }
  if (!wifiFlag)
  {
    Serial.println("\nConnection to WiFi failed, Connecting to LTE");
  }
}

void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.println("Message received from AWS:");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  StaticJsonDocument<200> doc;
  deserializeJson(doc, payload);
  String message = doc["message"];

  Serial.println(message);

  playSong(message);
}

void reconnect()
{
  while (!client.connected())
  {
    Serial.print("Attempting MQTT connection...");

    if (client.connect(THINGNAME))
    {
      Serial.println("connected");

      // Subscribe to the desired topic
      client.subscribe(topic);
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void sendATCommand(String command)
{
  Serial.print("Query:    ");
  Serial.println(command);
  LTE_Serial.println(command); // Send AT command
}

String batterySense()
{
  int sensorValue = analogRead(VP_PIN);

  float voltage = sensorValue * (3.4 / 4095.0) * 2; // Assuming 3.3V reference voltage

  Serial.print("Voltage: ");
  Serial.print(voltage);

  float batteryPercentage = ((voltage - minBatteryVolatage) / (maxBatteryVolatage - minBatteryVolatage)) * 100;
  Serial.print("      Battery Percentage: ");
  Serial.println(batteryPercentage);

  if (voltage <= 3.3)
  {
    return "batteryLOW";
  }
  else
  {
    return "batteryOK";
  }
}

const char *parseResponse(const char *response)
{
  const char *jsonStart = strchr(response, '{');

  if (jsonStart)
  {
    return jsonStart;
  }
  else
  {
    return nullptr;
  }
}

String processJsonMessage(const char *jsonString)
{
  if (jsonString)
  {
    StaticJsonDocument<400> jsonDoc;
    DeserializationError error = deserializeJson(jsonDoc, jsonString);
    if (!error)
    {
      const char *message = jsonDoc["message"];
      String temp(message);
      if (message)
      {
        Serial.print("Received message: ");
        Serial.println(message);
      }
      else
      {
        Serial.println("No message field in the JSON.");
      }
      return temp;
    }
    else
    {
      Serial.print("Failed to parse JSON: ");
      Serial.println(error.c_str());
    }
  }
  else
  {
    Serial.println("No JSON part found in the response.");
  }
  return "";
}

const char *parseLOCResponse(const char *response)
{
  const char *jsonStart = strchr(response, ',') + 1;
  if (jsonStart)
    return jsonStart;
  else
    return nullptr;
}

/**
 * Processes a JSON message by parsing it, extracting the "message" field, and returning
 * it as a String.
 *
 * @param jsonString a pointer to a character array that represents a JSON string.
 *
 * @return a String object.
 */

void checkLOC() // for getting the NEMA sentence from the GPS reciever
{
  sendATCommand("AT+QGPSGNMEA=\"GGA\"");
  delay(1000);
  receiveATCommand(3);
  delay(500);
}

float actualCoord(String value, int limit)
{
  float a = 0;
  float b = 0;
  int counter1 = 1;
  int counter2 = 10;
  int flag1 = 0;
  int flag2 = 0;
  for (int i = 0; i < value.length(); i++)
  {
    float temp = float(value[i] - '0');
    if (i < limit)
      a = a * 10 + temp;
    else
    {
      if (value[i] == '.')
      {
        flag1 = 1;
      }
      if (flag2 == 0 && flag1 == 0)
      {
        b = b * counter1 + temp;
        counter1 = counter1 * 10;
      }
    }

    if (flag2 == 1)
    {
      b = b + (temp / counter2);
      counter2 = counter2 * 10;
    }

    if (flag1 == 1)
    {
      flag2 = 1;
      flag1 = 0;
    }
  }
  return a + (b / 60.0);
}

/**
 * Takes a JSON string as input, extracts latitude and longitude values from it, converts them
 * to proper coordinates, and publishes the message with the extracted data to an MQTT topic.
 *
 * @param jsonString a pointer to a character array (string) that contains a JSON message.
 */

void Publish_Message(const char *jsonString)
{
  String output = "";
  StaticJsonDocument<96> doc;
  //                            <ddmm.mmmmm> <0ddmm,mmmmm>
  //+QGPSNMEA: $GPGGA,141009.00,2824.04883,N,07721.32483,E,0,00,99.99,179.9,M,,M,,*7E
  String Lat = "";
  String Long = "";
  String yaxis = "";
  String xaxis = "";

  while (*jsonString != ',')
    jsonString++;
  jsonString++;
  while (*jsonString != ',')
  {
    Lat = Lat + *jsonString;
    jsonString++;
  }
  jsonString++;
  while (*jsonString != ',')
  {
    yaxis = yaxis + *jsonString;
    jsonString++;
  }
  jsonString++;
  while (*jsonString != ',')
  {
    Long = Long + *jsonString;
    jsonString++;
  }
  jsonString++;
  while (*jsonString != ',')
  {
    xaxis = xaxis + *jsonString;
    jsonString++;
  }
  jsonString++;

  // always send 2 with lat and 3 with long for proper parsing
  float LLat = actualCoord(Lat, 2);
  float LLong = actualCoord(Long, 3);
  if (yaxis == "S")
    LLat = LLat * (-1);
  if (xaxis == "W")
    LLong = LLong * (-1);

  doc["DEVICE ID"] = DEVICE_ID;
  doc["LAT"] = LLat;
  doc["LONG"] = LLong;

  serializeJson(doc, output);
  int payloadSize = output.length();
  if(!wifiFlag)
  {
   sendATCommand("AT+QMTPUBEX=0,1,1,0,\"AWS/CIER/INFO/2\"," + String(payloadSize));
   delay(500);
   sendATCommand(output);
   delay(2000);
   receiveATCommand(0);
  }
  else if(wifiFlag)
  {
    client.publish(pub_topic, output.c_str());
  }
}

/**
 * Publishes a JSON payload to an MQTT topic using AT commands when the device is booted.
 */

void Publish_LIVE_NOW()
{
  String output = "";
  StaticJsonDocument<96> doc;

  doc["DEVICE ID"] = "2";
  doc["STATUS"] = "ACTIVE";

  serializeJson(doc, output);
  int payloadSize = output.length();

  if(!wifiFlag)
  {
  sendATCommand("AT+QMTPUBEX=0,1,1,0,\"AWS/CIER/INFO/2\"," + String(payloadSize));
  delay(500);
  sendATCommand(output);
  delay(2000);
  receiveATCommand(0);
  }
  else if(wifiFlag)
  {
    client.publish(pub_topic, output.c_str());
  }
}

void receiveATCommand(int flag)
{
  // flag=0 means Net configuration mode
  if (flag == 0)
  {
  retry:
    if (LTE_Serial.available())
    {
      String response1 = LTE_Serial.readStringUntil('\n');
      String response2 = LTE_Serial.readString();
      Serial.print("Response: ");
      Serial.println(response2);
      Serial.println("");
      if (response2 == "NO SIM")
      {
        Serial.println("Retrying query...");
        sendATCommand("AT+CPIN?");
        delay(1000);
        goto retry;
      }
    }
  }

  // flag=2 means AWS configuration mode
  else if (flag == 2)
  {
    if (LTE_Serial.available())
    {
      String response1 = LTE_Serial.readStringUntil('\n');
      String response2 = LTE_Serial.readString();
      Serial.print("Response: ");
      Serial.println(response2);
      Serial.println("");
      if (response2 == "ERROR")
      {
        ESP.restart();
      }
      if (response2.indexOf("+QMTSUB: 0,1,0,1") != -1)
      {
        Serial.println("Received the desired message!");
      }
      else
      {
        // If no data is received, print "retry"
        Serial.println("retry");
        delay(1000); // Adjust delay as needed
      }
    }
  }

  // flag=1 means permanent receive mode
  else if (flag == 1)
  {
    if (LTE_Serial.available())
    {
      String response2 = LTE_Serial.readString();
      Serial.print("Response: ");
      Serial.println(response2);
      Serial.println("");

      const char *jsonString = parseResponse(response2.c_str());
      String songName = processJsonMessage(jsonString);

      playSong(songName);
    }
  }
  else if (flag == 3)
  {
    if (LTE_Serial.available())
    {
      String response2 = LTE_Serial.readString();
      Serial.print("Response: ");
      Serial.println(response2);
      Serial.println("");
      const char *jsonString = parseLOCResponse(response2.c_str());
      Publish_Message(jsonString);
    }
  }
}

void checkBattery()
{
  String batteryval = batterySense();
  if (batteryval == "batteryLOW")
  {
    Serial.println("Battery LOW");
    mainFlag = 1;
    audio.connecttoFS(SD, "/THUNDERSTORM.mp3");
    warnUser = 1;
  }
}

void playSong(String songName)
{
  if (songName == "1")
  {
    mainFlag = 1;
    // playFile("/EARTHQUAKE",5000);
    audio.connecttoFS(SD, "/EARTHQUAKE.mp3");
    warnUser = 1;
  }
  if (songName == "2")
  {
    mainFlag = 1;
    audio.connecttoFS(SD, "/FLOOD.mp3");
    warnUser = 1;
  }
  if (songName == "3")
  {
    mainFlag = 1;
    audio.connecttoFS(SD, "/LANDSLIDE.mp3");
    warnUser = 1;
  }
  if (songName == "4")
  {
    mainFlag = 1;
    audio.connecttoFS(SD, "/LIGHTENINGSTRIKE.mp3");
    warnUser = 1;
  }
  if (songName == "5")
  {
    mainFlag = 1;
    audio.connecttoFS(SD, "/THUNDERSTORM.mp3");
    warnUser = 1;
  }
  if (songName == "LOC")
  {
    checkLOC();
  }
  if (songName == "STATUS")
  {
    Publish_LIVE_NOW();
  }
}

void connectToNet()
{
  sendATCommand("AT");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+CPIN?");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+CREG?");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+CGREG?");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+CSQ");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+COPS?");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+CGREG?");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  // sendATCommand("AT+QICSGP=1,1,\"airtelgprs.com\",\"\",\"\",0");
  // delay(1000);
  // receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QIACT=1");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QIACT?");
  delay(1000);
  receiveATCommand(0);
  delay(500);
}

void connectToAWS()
{
  sendATCommand("AT+QMTCFG=\"recv/mode\",0,0,1");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QMTCFG=\"SSL\",0,1,2");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QSSLCFG=\"cacert\",2,\"UFS:cacert.pem\"");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QSSLCFG=\"clientcert\",2,\"UFS:client.pem\"");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QSSLCFG=\"clientkey\",2,\"UFS:user_key.pem\"");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QSSLCFG=\"seclevel\",2,2");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QSSLCFG=\"sslversion\",2,4");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QSSLCFG=\"ciphersuite\",2,0xFFFF");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QSSLCFG=\"ignorelocaltime\",2,1");
  delay(1000);
  receiveATCommand(2);
  delay(500);
  sendATCommand("AT+QMTOPEN=0,\"a3egi4f3zufw8w-ats.iot.us-east-1.amazonaws.com\",8883");
  delay(1000);
  receiveATCommand(2);
  delay(500);
  sendATCommand("AT+QMTCONN=0,\"M26_0206\"");
  delay(5000);
  receiveATCommand(2);
  delay(500);
  sendATCommand("AT+QMTSUB=0,1,\"AWS/CIER/SUB/2\",1");
  delay(5000);
  receiveATCommand(2);
  delay(500);
  Serial.println("Entering into Receive state permanantly.....");

  Publish_LIVE_NOW();

  // playFile("/Anime powerup.m4r", 5000);
}

void connectToGPS()
{
  sendATCommand("AT+QGPSPOWER=1");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QGPS=1");
  delay(1000);
  receiveATCommand(0);
  delay(500);
  sendATCommand("AT+QGPSCFG=\"nmeasrc\",1");
  delay(1000);
  receiveATCommand(0);
  delay(500);
}

/**
 * The function connects to AWS and enters into a receive state permanently.
 */

void visualIndication()
{ // for giving visual notification through vibration motor and LED

  if (Capture == 1)
  {
    timeNow = millis();
    Capture = 0;
  }

  if (millis() <= timeNow + 500)
  {
    digitalWrite(VibraMotor, HIGH);
    digitalWrite(OnboardLED, LOW);
  }

  else if (millis() <= timeNow + 2000)
  {
    digitalWrite(VibraMotor, LOW);
    digitalWrite(OnboardLED, HIGH);
  }
  else if (millis() > timeNow + 2000)
  {
    Capture = 1;
  }
}

void setup()
{

  // Set microSD Card CS as OUTPUT and set HIGH
  Serial.begin(9600);
  LTE_Serial.begin(115200, SERIAL_8N1, EC_RX, EC_TX);
  analogReadResolution(12);
  pinMode(33, OUTPUT);
  pinMode(VibraMotor, OUTPUT);
  pinMode(UserSwitch, INPUT);
  pinMode(OnboardLED, OUTPUT);
  digitalWrite(33, HIGH);
  delay(1000);
  digitalWrite(33, LOW);
  delay(500);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // Initialize SPI bus for microSD Card
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  // Start microSD Card
  if (!SD.begin(SD_CS))
  {
    Serial.println("Error accessing microSD card!");
    while (true)
      ;
  }

  // Setup I2S
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

  // Set Volume
  audio.setVolume(21);

  connectToGPS();
  setupWiFi();

  // Set up AWS IoT callback function
  if (wifiFlag)
  {
    espClient.setCACert(AWS_CERT_CA);
    espClient.setCertificate(AWS_CERT_CRT);
    espClient.setPrivateKey(AWS_CERT_PRIVATE);

    client.setServer(AWS_IOT_ENDPOINT, 8883);
    client.setCallback(callback);
  }

  // Open music file
  if (!wifiFlag)
  {
    connectToNet();
    connectToAWS();
  }
}

void loop()
{
  if (wifiFlag)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      ESP.restart();
    }
    if (!client.connected())
    {
      reconnect();
    }

    delay(1000);
  }

  if (warnUser == 1)
  {
    visualIndication();
  }

  if (mainFlag == 0)
  {
    checkBattery();
    if (wifiFlag)
    {
      client.loop();
      audio.loop();
    }
    if (!wifiFlag)
    {
      receiveATCommand(1);
    }
  }
  else if (mainFlag == 1)
  {
    // Serial.println("LOOP RUNNING");
    audio.loop();
  }

  buttonState = digitalRead(UserSwitch);

  if (mainFlag == 1 && buttonState == LOW && lastButtonState == HIGH)
  {
    Serial.println("BUTTON STOP");
    audio.stopSong();
    mainFlag = 0;
    warnUser = 0;
    digitalWrite(VibraMotor, LOW);
    digitalWrite(OnboardLED, LOW);
    checkLOC();
  }
  lastButtonState = buttonState;

  // Functions here would transmit the location of the user if the user button is pressed for more than 4 seconds continuously
  if (buttonState == 0)
  {
    if (LPD == 0)
    {
      longPress = millis() + 4000;
      LPD = 1;
    }
    if (millis() >= longPress)
    {
      checkLOC();
      LPD = 0;
    }
  }
  if (buttonState == 1)
  {
    LPD = 0;
  }
}