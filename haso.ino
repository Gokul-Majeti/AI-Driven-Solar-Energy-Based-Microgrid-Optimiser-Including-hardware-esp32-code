#include <WiFi.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// --- WiFi & Server Config ---
const char* ssid = "Nothing Phone (3a) Pro_8669";
const char* password = "1122334455";
// Replace 192.168.X.X with the IPv4 Address you found in CMD
const char* server = "http://192.168.1.15:5000/update";

// --- Pin Definitions ---
const int RELAY_BATT_PIN  = 23; 
const int RELAY_GRID_PIN  = 19; 
const int CURR_BATT_PIN   = 32;
const int CURR_GRID_PIN   = 33;
const int VOLT_BATT_PIN   = 34;
const int VOLT_SOLAR_PIN  = 35;
const int LDR_PIN         = 25;

// --- I2C Pins for ESP32 ---
const int I2C_SDA = 21;
const int I2C_SCL = 22;

// --- Calibration Constants ---
const float V_DIV_RATIO     = 5.0; 
const float ADC_REF         = 3.3; 
const int   ADC_RES         = 4095;
const float ACS_SENSITIVITY = 0.185; 
const float ACS_OFFSET      = 1.65;    

// --- Thresholds ---
const float BATT_LOW_LIMIT  = 10.5;   
const int   LDR_NIGHT_THRES = 2000;  

// OLED Setup (SH110X)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define i2c_Address 0x3c 
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Time & Networking
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800); // IST Offset

void setup() {
  Serial.begin(115200);
  
  pinMode(RELAY_BATT_PIN, OUTPUT);
  pinMode(RELAY_GRID_PIN, OUTPUT);
  
  // Initial State: Safe Start (Grid On, Battery Off)
  digitalWrite(RELAY_BATT_PIN, HIGH);
  digitalWrite(RELAY_GRID_PIN, LOW);

  // Initialize I2C and Display
  Wire.begin(I2C_SDA, I2C_SCL);
  if(!display.begin(i2c_Address, true)) {
    Serial.println(F("SH110X allocation failed"));
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.println("Connecting WiFi...");
  display.display();

  // WiFi Connection
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  timeClient.begin();
  Serial.println("\nSystem Ready");
}

void loop() {
  timeClient.update();

  // 1. DATA ACQUISITION
  float vBatt  = (analogRead(VOLT_BATT_PIN) * ADC_REF / ADC_RES) * V_DIV_RATIO;
  float vSolar = (analogRead(VOLT_SOLAR_PIN) * ADC_REF / ADC_RES) * V_DIV_RATIO;
  int   ldrVal = analogRead(LDR_PIN);
  
  float iBatt = getACS712Current(CURR_BATT_PIN);
  float iGrid = getACS712Current(CURR_GRID_PIN);
  float totalCurrent = iBatt + iGrid;
  float powerWatts = (vBatt * iBatt) + (12.0 * iGrid);

  // 2. OPTIMIZATION LOGIC
  String sourceStatus = "";

  if (ldrVal > LDR_NIGHT_THRES) {
    digitalWrite(RELAY_BATT_PIN, HIGH); 
    digitalWrite(RELAY_GRID_PIN, LOW);  
    sourceStatus = "NIGHT: GRID";
  } 
  else if (vSolar > 11.0) {
    digitalWrite(RELAY_BATT_PIN, LOW);  
    digitalWrite(RELAY_GRID_PIN, HIGH); 
    sourceStatus = "DAY: SOLAR";
  } 
  else if (vBatt > BATT_LOW_LIMIT) {
    digitalWrite(RELAY_BATT_PIN, LOW);  
    digitalWrite(RELAY_GRID_PIN, HIGH); 
    sourceStatus = "DAY: BATT";
  } 
  else {
    digitalWrite(RELAY_BATT_PIN, HIGH);
    digitalWrite(RELAY_GRID_PIN, LOW);
    sourceStatus = "LOW BATT: GRID";
  }

  // 3. SEND TO DIGITAL TWIN (JSON)
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(server);
    http.addHeader("Content-Type", "application/json");

    String json = "{\"vBatt\":" + String(vBatt) + 
                  ",\"vSolar\":" + String(vSolar) + 
                  ",\"ldr\":" + String(ldrVal) + 
                  ",\"curr\":" + String(totalCurrent) + 
                  ",\"mode\":\"" + sourceStatus + "\"" +
                  ",\"time\":\"" + timeClient.getFormattedTime().substring(0,5) + "\"}";
    
    int httpResponseCode = http.POST(json);
    Serial.print("HTTP Response: "); Serial.println(httpResponseCode);
    http.end();
  }

  // 4. OUTPUT TO DISPLAY & SERIAL
  printToSerial(vBatt, vSolar, ldrVal, totalCurrent, sourceStatus);
  updateOLED(vBatt, vSolar, totalCurrent, powerWatts, sourceStatus);

  delay(5000); // 5-second cycle to prevent server spam
}

float getACS712Current(int pin) {
  long sum = 0;
  for(int i=0; i<20; i++) sum += analogRead(pin); 
  float avgVoltage = (sum / 20.0) * ADC_REF / ADC_RES;
  float current = (avgVoltage - ACS_OFFSET) / ACS_SENSITIVITY;
  return max(0.0f, abs(current)); 
}

void printToSerial(float vb, float vs, int ldr, float totalI, String mode) {
  Serial.println("--- Telemetry Sent ---");
  Serial.print("Mode: "); Serial.println(mode);
  Serial.print("Batt: "); Serial.print(vb); Serial.println("V");
}

void updateOLED(float vb, float vs, float totalI, float p, String mode) {
  display.clearDisplay();
  display.setCursor(0,0);
  
  display.println("MODE: " + mode);
  display.drawLine(0, 10, 128, 10, SH110X_WHITE);
  
  display.setCursor(0, 15);
  display.printf("BATT:  %.2f V\n", vb);
  display.printf("SOLAR: %.2f V\n", vs);
  display.printf("LOAD:  %.2f A\n", totalI);
  display.printf("POWER: %.2f W\n", p);
  
  int barWidth = map(constrain(vb, 10.0, 12.6) * 10, 100, 126, 0, 128);
  display.fillRect(0, 58, barWidth, 6, SH110X_WHITE);
  
  display.display();
}