/*
  Smart Table Tennis Bat - Bluetooth Data Collection System
  Arduino Nano 33 BLE Sense Rev2
  
  Features:
  - BMI270 IMU sampling at 100Hz
  - Swing detection with motion thresholds
  - 1-second data capture window
  - Bluetooth LE data transmission
  - OLED status display
  - JSON data formatting
*/

#include <Arduino_BMI270_BMM150.h>
#include <ArduinoBLE.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <ArduinoJson.h>

// OLED Display Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Sampling Configuration
#define SAMPLE_RATE_HZ 50         //changed to 50Hz to get constant rate
#define SAMPLE_INTERVAL_MS 20     //changed to follow 1000 / SAMPLE_RATE_HZ
#define PRE_TRIGGER_MS 100
#define POST_TRIGGER_MS 400
#define TOTAL_WINDOW_MS 500
#define PRE_TRIGGER_SAMPLES (PRE_TRIGGER_MS / SAMPLE_INTERVAL_MS)  // 10 samples
#define POST_TRIGGER_SAMPLES (POST_TRIGGER_MS / SAMPLE_INTERVAL_MS) // 40 samples
#define TOTAL_SAMPLES (TOTAL_WINDOW_MS / SAMPLE_INTERVAL_MS)        // 50 samples

// Detection Thresholds
#define ACCEL_THRESHOLD 2.0  // 2G
#define GYRO_THRESHOLD 200.0 // 200 degrees/second
#define COOLDOWN_MS 200      // 200ms between detections

// Data Structure for IMU readings
struct IMUData {
  unsigned long timestamp;
  float accelX, accelY, accelZ;
  float gyroX, gyroY, gyroZ;
};

// Circular buffer for continuous data collection (full 500ms window)
IMUData circularBuffer[TOTAL_SAMPLES];
int bufferIndex = 0;
bool bufferFull = false;

// Swing detection variables
bool swingDetected = false;
int triggerIndex = -1;
unsigned long lastSwingTime = 0;
unsigned long swingStartTime = 0;
unsigned long triggerTime = 0;
int swingCount = 0;
unsigned long sessionStartTime = 0;

// BLE Configuration
BLEService dataService("12345678-1234-1234-1234-123456789ABC");
BLECharacteristic swingCharacteristic("87654321-4321-4321-4321-CBA987654321", BLERead | BLENotify, 2048);

// Display and timing variables
unsigned long lastSampleTime = 0;
unsigned long lastDisplayUpdate = 0;
bool bleConnected = false;

void setup() {
  Serial.begin(115200);
  
  // Initialize display
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 allocation failed");
    for (;;); // Don't proceed, loop forever
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();
  
  // Initialize IMU
  if (!IMU.begin()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IMU Init Failed!");
    display.println("Press Reset");
    display.display();
    Serial.println("Failed to initialize IMU!");
    while (1);
  }
  
  // Initialize BLE
  if (!BLE.begin()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("BLE Init Failed!");
    display.println("Press Reset");
    display.display();
    Serial.println("Starting BLE failed!");
    while (1);
  }
  
  // Set up BLE
  BLE.setLocalName("TT-Stroke-Collector");
  BLE.setAdvertisedService(dataService);
  dataService.addCharacteristic(swingCharacteristic);
  BLE.addService(dataService);
  
  // Start advertising
  BLE.advertise();
  
  sessionStartTime = millis();
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("TT-Stroke-Collector");
  display.println("Waiting for");
  display.println("connection...");
  display.display();
  
  Serial.println("Arduino ready - waiting for BLE connection");
}

void loop() {
  // Handle BLE connections
  BLEDevice central = BLE.central();
  
  if (central && !bleConnected) {
    bleConnected = true;
    sessionStartTime = millis();
    swingCount = 0;
    Serial.println("Connected to central: " + String(central.address()));
    updateDisplay();
  }
  
  if (!central && bleConnected) {
    bleConnected = false;
    Serial.println("Disconnected from central");
    updateDisplay();
  }
  
  // Sample IMU at 100Hz
  if (millis() - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = millis();
    sampleIMU();
  }
  
  // Update display every 500ms
  if (millis() - lastDisplayUpdate >= 500) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}

void sampleIMU() {
  float ax, ay, az, gx, gy, gz;
  
  if (IMU.accelerationAvailable() && IMU.gyroscopeAvailable()) {
    IMU.readAcceleration(ax, ay, az);
    IMU.readGyroscope(gx, gy, gz);
    
    // Store in circular buffer (continuous collection)
    circularBuffer[bufferIndex].timestamp = millis();
    circularBuffer[bufferIndex].accelX = ax;
    circularBuffer[bufferIndex].accelY = ay;
    circularBuffer[bufferIndex].accelZ = az;
    circularBuffer[bufferIndex].gyroX = gx;
    circularBuffer[bufferIndex].gyroY = gy;
    circularBuffer[bufferIndex].gyroZ = gz;
    
    bufferIndex = (bufferIndex + 1) % TOTAL_SAMPLES;
    if (bufferIndex == 0) bufferFull = true;
    
    // Check for swing detection
    if (bleConnected && !swingDetected && (millis() - lastSwingTime > COOLDOWN_MS)) {
      float totalAccel = sqrt(ax*ax + ay*ay + az*az);
      float totalGyro = sqrt(gx*gx + gy*gy + gz*gz);
      
      if (totalAccel > ACCEL_THRESHOLD && totalGyro > GYRO_THRESHOLD) {
        swingDetected = true;
        triggerTime = millis();
        triggerIndex = (bufferIndex - 1 + TOTAL_SAMPLES) % TOTAL_SAMPLES; // Current sample index
        lastSwingTime = millis();
        Serial.println("Swing detected! Collecting data...");
      }
    }
    
    // Check if we have collected enough post-trigger data
    if (swingDetected && bleConnected) {
      if (millis() - triggerTime >= POST_TRIGGER_MS) {
        captureAndTransmitSwing();
        swingDetected = false;
        swingCount++;
      }
    }
  }
}

void captureAndTransmitSwing() {
  Serial.println("Transmitting swing data...");
  
  // Send swing header (compact format)
  String header = String(swingCount + 1) + "|" + String(triggerTime) + "|";
  swingCharacteristic.writeValue(header.c_str());
  delay(5);
  
  // Calculate start index for 500ms window
  int startIndex = (triggerIndex - PRE_TRIGGER_SAMPLES + TOTAL_SAMPLES) % TOTAL_SAMPLES;
  
  // Send data in compact format: timestamp,ax,ay,az,gx,gy,gz per line
  for (int i = 0; i < TOTAL_SAMPLES; i++) {
    int dataIndex = (startIndex + i) % TOTAL_SAMPLES;
    
    // Use relative timestamp
    int relativeTime = i * SAMPLE_INTERVAL_MS;
    
    String dataLine = String(relativeTime) + "," +
                     String(circularBuffer[dataIndex].accelX, 3) + "," +
                     String(circularBuffer[dataIndex].accelY, 3) + "," +
                     String(circularBuffer[dataIndex].accelZ, 3) + "," +
                     String(circularBuffer[dataIndex].gyroX, 1) + "," +
                     String(circularBuffer[dataIndex].gyroY, 1) + "," +
                     String(circularBuffer[dataIndex].gyroZ, 1) + "\n";
    
    swingCharacteristic.writeValue(dataLine.c_str());
    delay(2); // Minimal delay for BLE stability
  }
  
  // Send end marker
  swingCharacteristic.writeValue("END_SWING");
  
  Serial.println("Swing data transmitted - Swing #" + String(swingCount + 1));
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  
  if (!bleConnected) {
    display.println("TT-Stroke-Collector");
    display.println("Waiting for");
    display.println("connection...");
  } else {
    display.println("Connected!");
    display.print("Swings: ");
    display.println(swingCount);
    
    // Session time
    unsigned long sessionTime = (millis() - sessionStartTime) / 1000;
    display.print("Time: ");
    display.print(sessionTime / 60);
    display.print(":");
    if (sessionTime % 60 < 10) display.print("0");
    display.println(sessionTime % 60);
    
    // Status
    if (swingDetected) {
      display.println("Capturing...");
    } else {
      display.println("Ready");
    }
  }
  
  display.display();
}