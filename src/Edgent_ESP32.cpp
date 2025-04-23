#define BLYNK_TEMPLATE_ID "TMPL608a18IeD"
#define BLYNK_TEMPLATE_NAME "ESP32G8 SmartLock"

#define BLYNK_FIRMWARE_VERSION "0.1.0"

#define BLYNK_PRINT Serial
// #define BLYNK_DEBUG

#define APP_DEBUG

#include <Arduino.h>
#include <LittleFS.h>
#include <BlynkEdgent.h>
#include <ESP32Servo.h>
#include "ServoLock.h"
#include "DisplayManager.h"
#include "KeypadHandler.h"

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// DisplayManager setup
DisplayManager display(0x27, 16, 2); // LCD address 0x27, 16 cols, 2 rows

// Servo setup
const int SERVO_PIN = 13;                   // GPIO pin for servo control
ServoLock doorLock(SERVO_PIN, 0, 90, 5000); // Pin, lock position, unlock position, unlock duration

// Keypad pins
const uint8_t rowPins[4] = {14, 27, 26, 25}; // GIOP14, GIOP27, GIOP26, GIOP25
const uint8_t colPins[4] = {33, 32, 18, 19}; // GIOP33, GIOP32, GIOP18, GIOP19

// Create KeypadHandler instance
KeypadHandler keypadHandler(display, doorLock, rowPins, colPins);

// Default PIN if file doesn't exist
const String DEFAULT_PIN = "123456";

// Define task handles
TaskHandle_t keypadTaskHandle = NULL;

bool isValidPin(const String &pin)
{
  // Check length (must be exactly 6 characters)
  if (pin.length() != 6)
  {
    Serial.println("PIN validation failed: Invalid length");
    return false;
  }

  // Check if PIN contains only digits
  for (int i = 0; i < pin.length(); i++)
  {
    if (!isdigit(pin.charAt(i)))
    {
      Serial.println("PIN validation failed: Non-digit character found");
      return false;
    }
  }
  // If all checks pass, PIN is valid
  Serial.println("PIN validation passed");
  return true;
}

String readPinFromFile()
{
  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS Mount Failed. Using default PIN.");
    return DEFAULT_PIN;
  }

  // Check if pin file exists
  if (!LittleFS.exists("/pin.txt"))
  {
    // Create file with default PIN
    File pinFile = LittleFS.open("/pin.txt", FILE_WRITE);
    if (pinFile)
    {
      pinFile.print(DEFAULT_PIN);
      pinFile.close();
      Serial.println("Created pin.txt with default PIN");
    }
    else
    {
      Serial.println("Failed to create pin.txt");
    }
    return DEFAULT_PIN;
  }

  // Read existing PIN
  File pinFile = LittleFS.open("/pin.txt", FILE_READ);
  if (!pinFile)
  {
    Serial.println("Failed to open pin file. Using default PIN.");
    return DEFAULT_PIN;
  }

  String storedPin = pinFile.readStringUntil('\n');
  pinFile.close();

  storedPin.trim(); // Remove any whitespace

  // Validate PIN format and security
  if (!isValidPin(storedPin))
  {
    Serial.println("Invalid PIN format in file. Using default PIN.");
    return DEFAULT_PIN;
  }

  Serial.println("PIN loaded successfully");
  return storedPin;
}

bool changePin(const String &newPin)
{
  // First validate the new PIN
  if (!isValidPin(newPin))
  {
    Serial.println("Cannot change PIN: New PIN is invalid");
    return false;
  }

  // Write new PIN to file
  File pinFile = LittleFS.open("/pin.txt", FILE_WRITE);
  if (!pinFile)
  {
    Serial.println("Failed to open pin file for writing. PIN not changed.");
    return false;
  }

  pinFile.print(newPin);
  pinFile.close();
  Serial.println("PIN changed successfully");
  return true;
}

// Keypad task function - runs on core 1
void keypadTask(void *parameter)
{
  for (;;)
  {
    keypadHandler.processKeyInput();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

BLYNK_WRITE(V0)
{
  // Handle Blynk button press to unlock the door
  if (param.asInt())
  {
    display.showMessage("Door Unlocked", "Blynk Command", 0);
    doorLock.unlockTemporarily();

    keypadHandler.clearPasscodeDisplay(); // Clear passcode display after unlocking
  }
}

BLYNK_WRITE(V1)
{
  String newPin = param.asStr();
  Serial.println("New PIN received from Blynk: " + newPin);

  if (changePin(newPin))
  {
    // Update the keypadHandler with the new PIN
    keypadHandler.init(newPin);
    display.showMessage("PIN Changed", "Successfully", 4000);
  }
  else
  {
    display.showMessage("PIN Change", "Failed", 3000);
  }
  keypadHandler.clearPasscodeDisplay();
}

void setup()
{

  Serial.begin(115200);
  delay(300);

  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  else
  {
    Serial.println("Little FS Mounted Successfully");
  }

  // Setup the LCD
  display.init();

  delay(300);
  // Display initial message
  display.showMessage("Initializing...", "Please wait");

  // Initialize BlynkEdgent
  BlynkEdgent.begin();
  delay(300);

  // Initialize the servo lock
  doorLock.init();
  delay(300);

  // Read PIN from file and initialize keypad handler
  String pin = readPinFromFile();
  keypadHandler.init(pin);
  delay(300);

  // Create Keypad task on core 1
  xTaskCreatePinnedToCore(
      keypadTask,        /* Function to implement the task */
      "KeypadTask",      /* Name of the task */
      4096,              /* Stack size in words */
      NULL,              /* Task input parameter */
      1,                 /* Priority of the task */
      &keypadTaskHandle, /* Task handle */
      1                  /* Core where the task should run */
  );
  Serial.println("Smart Lock system initialized");
}

void loop()
{
  BlynkEdgent.run();
  delay(1000);
}