#include <WiFi.h>
#include <Arduino.h>
#include "FS.h"
#include "SPIFFS.h"

unsigned long totalPumpOnTime = 0;

// WiFi Access Point credentials
const char *ssid = "Sem6_project";
const char *password = "12345678";

// Moisture sensor and control pin
const int sensorPin = 34;
const int controlPin = 4;

WiFiServer server(80);
bool gpioState = false;
bool autoMode = false;

// Thresholds
int lowerThreshold = 30;
int upperThreshold = 70;

// Log storage
#define MAX_LOGS 10
String moistureLogs[MAX_LOGS];
int logIndex = 0;

// Global variables to share sensor data
int mapMoisture = 0;
float currentVoltage = 0.0;

// ---------- Save/load thresholds ----------
void saveThresholds() {
  File file = SPIFFS.open("/thresholds.txt", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to save thresholds.");
    return;
  }
  file.println(String(lowerThreshold));
  file.println(String(upperThreshold));
  file.close();
}

void loadThresholds() {
  File file = SPIFFS.open("/thresholds.txt", FILE_READ);
  if (!file) {
    Serial.println("No saved thresholds found. Using defaults.");
    return;
  }
  String low = file.readStringUntil('\n');
  String high = file.readStringUntil('\n');
  lowerThreshold = low.toInt();
  upperThreshold = high.toInt();
  Serial.println("Loaded thresholds from SPIFFS:");
  Serial.println("Lower: " + String(lowerThreshold) + "  Upper: " + String(upperThreshold));
  file.close();
}

// ---------- Send JSON data (example if you want to expand) ----------
void sendJsonData(WiFiClient client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();

  client.print("{");
  client.printf("\"moisturePercentage\": %d,", mapMoisture);
  client.printf("\"voltage\": %.2f,", currentVoltage);
  client.printf("\"gpioState\": %s,", gpioState ? "true" : "false");
  client.printf("\"autoMode\": %s,", autoMode ? "true" : "false");

  client.print("\"logs\":[");
  for (int i = 0; i < MAX_LOGS; i++) {
    int idx = (logIndex + i) % MAX_LOGS;
    client.print("\"" + moistureLogs[idx] + "\"");
    if (i < MAX_LOGS - 1) client.print(",");
  }
  client.println("]");
  client.println("}");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(sensorPin, INPUT);
  pinMode(controlPin, OUTPUT);
  digitalWrite(controlPin, HIGH);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed!");
    return;
  }

  loadThresholds();  // Load saved thresholds

  WiFi.softAP(ssid, password);
  Serial.println("Access Point Started");
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());

  server.begin();
}

// ---------- Save logs ----------
void saveLogToFile(String logEntry) {
  File file = SPIFFS.open("/moisture_log.csv", FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open log file for writing.");
    return;
  }

  if (file.size() == 0) {
    file.println("Time (ms),Moisture (%),Pump ON Duration (ms),Moisture Status");
  }
  file.println(logEntry);
  file.close();
}

// ---------- Download logs ----------
void handleDownload(WiFiClient client) {
  File file = SPIFFS.open("/moisture_log.csv", FILE_READ);
  if (!file) {
    client.println("HTTP/1.1 500 Internal Server Error");
    client.println("Content-Type: text/plain");
    client.println();
    client.println("Failed to open log file.");
    return;
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/csv");
  client.println("Content-Disposition: attachment; filename=moisture_log.csv");
  client.print("Content-Length: ");
  client.println(file.size());
  client.println("Connection: close");
  client.println();

  uint8_t buffer[128];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    client.write(buffer, bytesRead);
  }
  file.close();
}

// ---------- Main loop ----------
void loop() {
  WiFiClient client = server.available();
  if (!client) return;

  Serial.println("New Client Connected");
  unsigned long timeout = millis() + 2000;
  while (!client.available() && millis() < timeout) {
    delay(10);
  }

  if (!client.available()) {
    Serial.println("Client Disconnected (Timeout)");
    client.stop();
    return;
  }

  String request = client.readStringUntil('\r');
  client.readStringUntil('\n'); // skip the rest of the line
  Serial.println("Request: " + request);

  if (request.indexOf("GET /download") != -1) {
    handleDownload(client);
    client.stop();
    Serial.println("Client Disconnected");
    return;
  }

  if (request.indexOf("/startPump") != -1) {
    gpioState = true;
    digitalWrite(controlPin, LOW);
  }

  if (request.indexOf("/stopPump") != -1) {
    gpioState = false;
    digitalWrite(controlPin, HIGH);
  }

  if (request.indexOf("/auto") != -1) {
    autoMode = !autoMode;
  }

  if (request.indexOf("/setThresholds") != -1) {
    int lowIndex = request.indexOf("low=");
    int highIndex = request.indexOf("high=");
    if (lowIndex != -1 && highIndex != -1) {
      int lowEnd = request.indexOf('&', lowIndex);
      if (lowEnd == -1) lowEnd = request.length();
      lowerThreshold = request.substring(lowIndex + 4, lowEnd).toInt();
      upperThreshold = request.substring(highIndex + 5).toInt();
      saveThresholds();  // Save new values
    }
  }

  // Read sensor
  int sensorValue = analogRead(sensorPin);
  // Calculate voltage in mV
  currentVoltage = sensorValue * (3.3 / 4095.0) * 1000.0;
  currentVoltage = max(currentVoltage, 0.0f);

  // Map voltage to moisture %
  // Adjust these values to your sensor calibration
  mapMoisture = map(currentVoltage, 2800, 1300, 0, 100);
  mapMoisture = constrain(mapMoisture, 0, 100);

  String moistureStatus = (mapMoisture < 50) ? "Low" : "High";

  // Update logs array
  String logEntry = "Time: " + String(millis()) + ",Moisture: " + String(mapMoisture) + ", Pump on Duration: " + String(totalPumpOnTime) + ", Status: " + moistureStatus;
  moistureLogs[logIndex] = logEntry;
  logIndex = (logIndex + 1) % MAX_LOGS;
  saveLogToFile(logEntry);

  // Auto mode control
  if (autoMode) {
    if (mapMoisture < lowerThreshold) {
      if (!gpioState) {
        gpioState = true;
        digitalWrite(controlPin, LOW);
        Serial.println("Pump ON due to low moisture");
      }
    } else if (mapMoisture > upperThreshold) {
      if (gpioState) {
        gpioState = false;
        digitalWrite(controlPin, HIGH);
        Serial.println("Pump OFF due to sufficient moisture");
      }
    }
  }

  // ---------- HTML response ----------
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head><title>ESP32 Soil Moisture</title>");
  client.println("<meta http-equiv='refresh' content='5'>");
  client.println("<style>");
  client.println("body { font-family: 'Segoe UI', sans-serif; background-color: #1e1e1e; color: #e0e0e0; margin: 0; padding: 0; }");
  client.println(".container { max-width: 800px; margin: auto; padding: 20px; }");
  client.println("h1 { color: #FF3C3C; font-size: 48px; text-align: center; }");
  client.println(".card { background-color: #2c2c2c; border-radius: 10px; padding: 20px; margin-top: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.5); }");
  client.println(".button { padding: 12px 24px; margin: 10px 5px; border: none; border-radius: 5px; font-size: 18px; cursor: pointer; transition: 0.3s; }");
  client.println(".start { background-color: #2196F3; color: white; }");
  client.println(".stop { background-color: #f44336; color: white; }");
  client.println(".auto { background-color: #FFC107; color: black; }");
  client.println(".download { background-color: #4CAF50; color: white; }");
  client.println(".button:hover { opacity: 0.8; }");
  client.println(".progress-bar { width: 100%; background-color: #444; height: 30px; border-radius: 5px; margin-top: 10px; }");
  client.println(".progress { height: 100%; border-radius: 5px; }");
  client.println(".log-container { background-color: #222; padding: 20px; margin-top: 20px; border-radius: 8px; }");
  client.println(".log-container p { font-size: 14px; font-family: monospace; color: #bbb; margin: 5px 0; }");
  client.println("</style></head><body>");
  client.println("<div class='container'>");
  client.println("<h1>ESP32 Soil Monitor</h1>");

  client.println("<div class='card'>");
  client.println("<h2>Sensor Data</h2>");
  client.print("<p>Moisture Level: <strong>");
  client.print(String(mapMoisture));
  client.println("%</strong></p>");
  client.print("<p>Sensor Voltage: <strong>");
  client.print(String(currentVoltage));
  client.println(" mV</strong></p>");
  client.print("<div class='progress-bar'><div class='progress' style='width:");
  client.print(String(mapMoisture));
  client.print("%; background-color:");
  client.print(mapMoisture < lowerThreshold ? "#f44336" : "#8bc34a");
  client.println(";'></div></div>");
  client.println("</div>");

  client.println("<div class='card'>");
  client.println("<h2>Pump Control</h2>");
  client.print("<p>Status: <strong id='pumpStatus'>");
  client.print(gpioState ? "ON" : "OFF");
  client.println("</strong></p>");

  client.print("<p>Auto Mode Status: <strong id='autoModeStatus'>");
  client.print(autoMode ? "Auto Mode ON" : "Auto Mode OFF");
  client.println("</strong></p>");

  client.println("<button class='button start' onclick=\"startPump()\">Start Pump</button>");
  client.println("<button class='button stop' onclick=\"stopPump()\">Stop Pump</button>");
  client.println("<button class='button auto' onclick=\"toggleAuto()\">Toggle Auto Mode</button>");
  client.println("<a href='/download' target='_blank'><button class='button download'>Download Log File</button></a>");
  client.println("</div>");

  client.println("<div class='card'>");
  client.println("<h2>Set Thresholds</h2>");
  client.println("<form action='/setThresholds' method='GET'>");
  client.print("<label>Lower Threshold (%):</label><input type='number' name='low' value='");
  client.print(lowerThreshold);
  client.println("' min='0' max='100'><br>");
  client.print("<label>Upper Threshold (%):</label><input type='number' name='high' value='");
  client.print(upperThreshold);
  client.println("' min='0' max='100'><br>");
  client.println("<input class='button auto' type='submit' value='Set Thresholds'>");
  client.println("</form>");
  client.println("</div>");

  client.println("<div class='card log-container'>");
  client.println("<h2>Last 10 Logs</h2>");
  for (int i = 0; i < MAX_LOGS; i++) {
    int index = (logIndex + i) % MAX_LOGS;
    client.print("<p>");
    client.print(moistureLogs[index]);
    client.println("</p>");
  }
  client.println("</div>");

  client.println("<script>");
  client.println("function startPump() { var xhr = new XMLHttpRequest(); xhr.open('GET', '/startPump', true); xhr.send(); }");
  client.println("function stopPump() { var xhr = new XMLHttpRequest(); xhr.open('GET', '/stopPump', true); xhr.send(); }");
  client.println("function toggleAuto() { var xhr = new XMLHttpRequest(); xhr.open('GET', '/auto', true); xhr.send(); }");
  client.println("</script>");

  client.println("</div></body></html>");

  client.stop();
  Serial.println("Client Disconnected");
}
