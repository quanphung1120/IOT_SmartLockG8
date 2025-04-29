#define BLYNK_TEMPLATE_ID "TMPL608a18IeD"
#define BLYNK_TEMPLATE_NAME "ESP32G8 SmartLock"
#define BLYNK_FIRMWARE_VERSION "0.1.0"
#define BLYNK_PRINT Serial
#define APP_DEBUG
#define DEFAULT_PIN "123456"

#include <Adafruit_Fingerprint.h>
#include <Arduino.h>
#include <BlynkEdgent.h>
#include <ESP32Servo.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

#include <atomic>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Hardware initialization
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);
LiquidCrystal_I2C lcd(0x27, 16, 2);
std::mutex displayMutex;
Servo lockServo;
Preferences prefs;

// Pin configurations
const int SERVO_PIN = 13;
const int MOVEMENT_PIN = 4;
const uint8_t rowPins[4] = {14, 27, 26, 25};
const uint8_t colPins[4] = {33, 32, 18, 19};
char keyMap[4][4] = {{'1', '2', '3', 'A'},
                     {'4', '5', '6', 'B'},
                     {'7', '8', '9', 'C'},
                     {'*', '0', '#', 'D'}};

// System constants
const int LOCK_POSITION = 0;
const int UNLOCK_POSITION = 90;
const unsigned long UNLOCK_DURATION = 15000;  // 15 seconds for auto-lock
const unsigned long LOCKOUT_DURATION =
    60000;  // 1 minute lockout after 3 failures
const unsigned long BACKLIGHT_TIMEOUT = 30000;  // 30 seconds for LCD backlight
const unsigned long KEYPAD_TIMEOUT = 30000;     // 30 seconds for keypad input
const uint8_t PASSCODE_LENGTH = 6;

// System state
Keypad keypad =
    Keypad(makeKeymap(keyMap), (byte *)rowPins, (byte *)colPins, 4, 4);
TaskHandle_t inputTaskHandle = NULL;
String unlockPin;
char currentPasscode[PASSCODE_LENGTH + 1] = {0};
int passcodeIndex = 0;
int pinFailedAttempts = 0;
int fingerFailedAttempts = 0;
bool isLocked = true;
bool backlightEnabled = true;
bool autoLockPending = false;

unsigned long autoLockTime = 0;
unsigned long lockoutUntil = 0;
unsigned long lastKeyPressTime = 0;

unsigned long lastPinFailTime = 0;
unsigned long lastFingerFailTime = 0;
const unsigned long ATTEMPT_RESET_TIME = 120000;  // 2 minutes in milliseconds

// Using atomic because this variable is accessed from multiple threads
std::atomic<bool> isRegistering{false};

// Display functions
void displayUpdate(uint8_t col, uint8_t row, const String &text,
                   bool clearFirst = false) {
    std::lock_guard<std::mutex> lock(displayMutex);
    if (clearFirst) lcd.clear();
    lcd.setCursor(col, row);
    lcd.print(text);
}

void displayMessage(const String &line1, const String &line2 = "",
                    uint16_t displayTime = 0) {
    std::lock_guard<std::mutex> lock(displayMutex);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1);

    if (line2.length() > 0) {
        lcd.setCursor(0, 1);
        lcd.print(line2);
    }

    if (displayTime > 0) delay(displayTime);
}

// Lock control functions
unsigned long lastServoCommandTime = 0;
const unsigned long SERVO_COMMAND_DEBOUNCE = 500;  // 500ms between commands

// Update setLockPosition
void setLockPosition(bool lock) {
    // Only move if state is changing
    if (isLocked != lock) {
        lockServo.write(lock ? LOCK_POSITION : UNLOCK_POSITION);
        isLocked = lock;
        Serial.println(lock ? "Door locked" : "Door unlocked");
    }
}

void unlockTemporarily() {
    setLockPosition(false);
    autoLockTime = millis() + UNLOCK_DURATION;
    autoLockPending = true;

    displayUpdate(0, 0, "Door Unlocked", true);
    displayUpdate(0, 1, "Locks in: " + String(UNLOCK_DURATION / 1000) + "s");
}

bool isLockoutActive() { return millis() < lockoutUntil; }

// PIN management
bool isValidPin(const String &pin) {
    if (pin.length() != 6) return false;
    for (char c : pin)
        if (!isdigit(c)) return false;
    return true;
}

String loadPin() {
    prefs.begin("smartlock", true);
    String pin = prefs.getString("pin", DEFAULT_PIN);
    prefs.end();

    return isValidPin(pin) ? pin : DEFAULT_PIN;
}

bool savePin(const String &newPin) {
    if (!isValidPin(newPin)) return false;

    if (prefs.begin("smartlock", false)) {
        prefs.putString("pin", newPin);
        prefs.end();

        Serial.println("PIN saved: " + newPin);
        unlockPin = newPin;
        return true;
    } else {
        Serial.println("Failed to save PIN to preferences");
        return false;
    }
}

void sendBlynkEvent(const char *eventName, const char *eventDescription) {
    if (Blynk.connected()) {
        Blynk.logEvent(eventName, eventDescription);
        Serial.println("Event sent: " + String(eventName));
    } else {
        Serial.println("Blynk offline, can't send event: " + String(eventName));
    }
}

// Keypad functions
void resetPasscodeEntry() {
    passcodeIndex = 0;
    memset(currentPasscode, 0, sizeof(currentPasscode));
    displayUpdate(0, 0, "Enter Passcode:", true);
    displayUpdate(5, 1, "______");
}

bool processKeypadInput() {
    // Check for timeout
    if (passcodeIndex > 0 && millis() - lastKeyPressTime >= KEYPAD_TIMEOUT) {
        displayMessage("Timeout", "Input cleared", 1500);
        resetPasscodeEntry();
        return false;
    }

    char key = keypad.getKey();
    if (!key) return false;

    lastKeyPressTime = millis();

    // Handle digit keys (0-9)
    if (isdigit(key) && passcodeIndex < PASSCODE_LENGTH) {
        currentPasscode[passcodeIndex] = key;
        displayUpdate(5 + passcodeIndex, 1, String(key));
        passcodeIndex++;

        if (passcodeIndex == PASSCODE_LENGTH) {
            displayUpdate(0, 0, "Press # to verify", false);
        }
        return false;
    }

    // Handle backspace (*)
    if (key == '*' && passcodeIndex > 0) {
        passcodeIndex--;
        currentPasscode[passcodeIndex] = 0;
        displayUpdate(5 + passcodeIndex, 1, "_");
        return false;
    }

    // Handle enter key (#)
    if (key == '#' && passcodeIndex == PASSCODE_LENGTH) {
        if (String(currentPasscode) == unlockPin) {
            pinFailedAttempts = 0;
            sendBlynkEvent("access_granted", "Access granted via passcode");
            unlockTemporarily();
            displayMessage("Access Granted!", "Door Unlocked", 2000);
            resetPasscodeEntry();
            return true;
        } else {
            pinFailedAttempts++;
            lastPinFailTime = millis();

            if (pinFailedAttempts >= 3) {
                sendBlynkEvent("send_alarm",
                               "Access denied, too many attempts");

                displayMessage("Too Many Attempts");
                lockoutUntil = millis() + LOCKOUT_DURATION;
                pinFailedAttempts = 0;
            } else {
                sendBlynkEvent("access_denied", "Access denied via passcode");
                displayMessage("Access Denied!",
                               String(3 - pinFailedAttempts) + " attempts left",
                               2000);
            }
            resetPasscodeEntry();
            return false;
        }
    }

    return false;
}

// Fingerprint functions
uint8_t findAvailableFingerID() {
    finger.getTemplateCount();

    for (uint8_t id = 1; id < 128; id++) {
        uint8_t p = finger.loadModel(id);
        if (p == FINGERPRINT_BADLOCATION) {
            return id;  // This ID is available
        }
    }

    return 0;  // No available IDs
}

int getFingerprintIDez() {
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK) return -1;

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) return -1;

    p = finger.fingerFastSearch();
    if (p != FINGERPRINT_OK) return -2;

    Serial.print("Found ID #");
    Serial.print(finger.fingerID);
    Serial.print(" with confidence of ");
    Serial.println(finger.confidence);
    return finger.fingerID;
}

bool getFingerprintEnroll() {
    int p = -1;
    int id = findAvailableFingerID();
    int attemptCount = 0;
    const int MAX_ATTEMPTS = 5;

    if (id == 0) {
        displayMessage("No free slots", "Clear database first", 2000);
        return false;
    }

    displayMessage("Enrolling ID #" + String(id), "Place finger");
    Serial.println("Waiting for valid finger to enroll as #" + String(id));

    // First image capture with timeout
    attemptCount = 0;
    while (p != FINGERPRINT_OK && attemptCount < MAX_ATTEMPTS) {
        p = finger.getImage();
        switch (p) {
            case FINGERPRINT_OK:
                displayMessage("Image taken", "Processing...");
                Serial.println("Image taken");
                break;
            case FINGERPRINT_NOFINGER:
                delay(100);
                break;
            case FINGERPRINT_PACKETRECIEVEERR:
                displayMessage("Comm error", "Try again");
                Serial.println("Communication error");
                attemptCount++;
                delay(500);
                break;
            case FINGERPRINT_IMAGEFAIL:
                displayMessage("Imaging error", "Try again");
                Serial.println("Imaging error");
                attemptCount++;
                delay(500);
                break;
            default:
                displayMessage("Unknown error", String(p));
                Serial.println("Unknown error");
                attemptCount++;
                delay(500);
                break;
        }

        if (attemptCount >= MAX_ATTEMPTS) {
            displayMessage("Too many errors", "Registration canceled", 2000);
            return false;
        }
    }

    // Process first image
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
        String errorMsg = "Processing failed";
        switch (p) {
            case FINGERPRINT_IMAGEMESS:
                errorMsg = "Image too messy";
                break;
            case FINGERPRINT_PACKETRECIEVEERR:
                errorMsg = "Communication error";
                break;
            case FINGERPRINT_FEATUREFAIL:
            case FINGERPRINT_INVALIDIMAGE:
                errorMsg = "No features found";
                break;
        }
        displayMessage(errorMsg, "Try again", 2000);
        return false;
    }

    // Wait for finger removal
    displayMessage("Remove finger", "from sensor");
    Serial.println("Remove finger");
    delay(1000);
    p = 0;

    // Wait for finger removal with timeout
    attemptCount = 0;
    while (p != FINGERPRINT_NOFINGER && attemptCount < MAX_ATTEMPTS) {
        p = finger.getImage();
        if (p != FINGERPRINT_NOFINGER) {
            delay(1000);
            attemptCount++;
        }

        if (attemptCount >= MAX_ATTEMPTS) {
            displayMessage("Finger not removed", "Registration canceled", 2000);
            return false;
        }
    }

    // Second fingerprint capture
    displayMessage("Place same", "finger again");
    Serial.println("Place same finger again for ID #" + String(id));
    p = -1;

    // Second image capture with timeout
    attemptCount = 0;
    while (p != FINGERPRINT_OK && attemptCount < MAX_ATTEMPTS) {
        p = finger.getImage();
        switch (p) {
            case FINGERPRINT_OK:
                displayMessage("Image taken", "Processing...");
                Serial.println("Image taken");
                break;
            case FINGERPRINT_NOFINGER:
                delay(100);
                break;
            case FINGERPRINT_PACKETRECIEVEERR:
                displayMessage("Comm error", "Try again");
                Serial.println("Communication error");
                attemptCount++;
                delay(500);
                break;
            case FINGERPRINT_IMAGEFAIL:
                displayMessage("Imaging error", "Try again");
                Serial.println("Imaging error");
                attemptCount++;
                delay(500);
                break;
            default:
                displayMessage("Unknown error", String(p));
                Serial.println("Unknown error");
                attemptCount++;
                delay(500);
                break;
        }

        if (attemptCount >= MAX_ATTEMPTS) {
            displayMessage("Too many errors", "Registration canceled", 2000);
            return false;
        }
    }

    // Process second image
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
        String errorMsg = "Processing failed";
        switch (p) {
            case FINGERPRINT_IMAGEMESS:
                errorMsg = "Image too messy";
                break;
            case FINGERPRINT_PACKETRECIEVEERR:
                errorMsg = "Communication error";
                break;
            case FINGERPRINT_FEATUREFAIL:
            case FINGERPRINT_INVALIDIMAGE:
                errorMsg = "No features found";
                break;
        }
        displayMessage(errorMsg, "Try again", 2000);
        return false;
    }

    // Create model from two fingerprints
    displayMessage("Creating model", "Please wait");
    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
        if (p == FINGERPRINT_ENROLLMISMATCH) {
            displayMessage("Fingers didn't", "match - Try again", 2000);
        } else {
            displayMessage("Model error", String(p), 2000);
        }
        return false;
    }

    // Store the model
    displayMessage("Storing as ID #" + String(id), "Please wait");
    p = finger.storeModel(id);
    if (p == FINGERPRINT_OK) {
        displayMessage("Success!", "Fingerprint stored", 2000);
        return true;
    } else {
        String errorMsg = "Storage failed";
        switch (p) {
            case FINGERPRINT_PACKETRECIEVEERR:
                errorMsg = "Communication error";
                break;
            case FINGERPRINT_BADLOCATION:
                errorMsg = "Invalid location";
                break;
            case FINGERPRINT_FLASHERR:
                errorMsg = "Flash write error";
                break;
        }
        displayMessage(errorMsg, "Try again", 2000);
        return false;
    }
}

void handleFingerprint() {
    if (!isRegistering && isLocked && !isLockoutActive()) {
        int fingerID = getFingerprintIDez();

        while (finger.getImage() != FINGERPRINT_NOFINGER) {
            displayMessage("Remove your", "finger to process");
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        if (fingerID > 0) {
            // Wait for finger removal before unlocking
            fingerFailedAttempts = 0;
            displayMessage("Access Granted!", "Door Unlocked", 2000);
            unlockTemporarily();

            sendBlynkEvent("access_granted", "Access granted via fingerprint");
        } else if (fingerID == -2) {
            sendBlynkEvent("access_denied", "Access denied via fingerprint");

            displayMessage("Access Denied!", "", 2000);
            fingerFailedAttempts++;
            resetPasscodeEntry();

            if (fingerFailedAttempts >= 5) {
                sendBlynkEvent("send_alarm",
                               "Access denied, too many attempts");

                displayMessage("Too Many Attempts", "Locking out", 2000);
                lockoutUntil = millis() + LOCKOUT_DURATION;
                fingerFailedAttempts = 0;
            }
        }
    }
}

// Main input task
void inputTask(void *parameter) {
    // Serial.println("inputTask() running on core " +
    // String(xPortGetCoreID()));
    int fingerScanCounter = 0;
    int pinStateCurrent = digitalRead(MOVEMENT_PIN);
    int pinStatePrevious = pinStateCurrent;
    long lastDisplayedTime = -1;

    for (;;) {
        if (isLockoutActive()) {
            unsigned long currentTime = millis();
            unsigned long remainingSecs = (lockoutUntil - currentTime) / 1000;
            displayMessage("System Locked",
                           String(remainingSecs) + "s remaining");

            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        if (autoLockPending) {
            // Check motion sensor
            pinStatePrevious = pinStateCurrent;
            pinStateCurrent = digitalRead(MOVEMENT_PIN);

            if (pinStatePrevious == LOW && pinStateCurrent == HIGH) {
                Serial.println("Motion detected!");
                if (!isLocked && autoLockPending) {
                    autoLockTime = millis() + UNLOCK_DURATION;
                }
            }

            // Update countdown display
            long remainingTime = (autoLockTime - millis()) / 1000;
            if (remainingTime != lastDisplayedTime && remainingTime >= 0) {
                lastDisplayedTime = remainingTime;
                displayMessage("System Locked",
                               String(remainingTime) + "s remaining");
            }

            if (!autoLockPending) {
                // Reset display if the door is locked
                lastDisplayedTime = -1;
            }
        }

        // Check if it's time to auto close the door
        if (autoLockPending && millis() >= autoLockTime) {
            setLockPosition(true);
            autoLockPending = false;
            displayMessage("Door Locked", "Auto-lock complete", 1000);
            resetPasscodeEntry();
            vTaskDelay(1000);
        }

        if (pinFailedAttempts > 0 &&
            (millis() - lastPinFailTime) > ATTEMPT_RESET_TIME) {
            Serial.println("PIN failed attempts reset after 2 minutes");
            pinFailedAttempts = 0;
        }

        // Reset fingerprint attempts if needed
        if (fingerFailedAttempts > 0 &&
            (millis() - lastFingerFailTime) > ATTEMPT_RESET_TIME) {
            Serial.println("Fingerprint failed attempts reset after 2 minutes");
            fingerFailedAttempts = 0;
        }

        // Process keypad input
        if (!isLocked) {
            continue;
        }

        if (processKeypadInput()) {
            continue;  // Successful entry, skip to next iteration
        }

        // Check fingerprint sensor (every 5 iterations)
        fingerScanCounter++;
        if (fingerScanCounter >= 5) {
            fingerScanCounter = 0;
            handleFingerprint();
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// Blynk handlers
BLYNK_WRITE(V0) {
    if (param.asInt()) {
        displayMessage("Door Unlocked", "Blynk Command");
        lockoutUntil = 0;
        unlockTemporarily();
        resetPasscodeEntry();
    }
}

BLYNK_WRITE(V1) {
    String newPin = param.asStr();

    if (!isValidPin(newPin)) {
        // return the message to blynk app
        return;
    }

    if (savePin(newPin)) {
        displayMessage("PIN Changed", "Successfully", 4000);
        // // Send confirmation to Blynk
        // Blynk.virtualWrite(V3, "PIN changed: " + String(millis()));
    } else {
        displayMessage("PIN Change", "Failed", 3000);
    }
    resetPasscodeEntry();
}

BLYNK_WRITE(V2) {
    if (param.asInt()) {
        if (isRegistering) {
            return;
        }

        isRegistering = true;
        bool success = getFingerprintEnroll();
        isRegistering = false;

        if (success) {
            displayMessage("Registration", "Successfully", 2000);
        } else {
            displayMessage("Registration", "Failed", 2000);
        }

        resetPasscodeEntry();
    }
}

BLYNK_WRITE(V9) {
    if (param.asInt()) {
        displayMessage("Clearing", "Fingerprint Data", 2000);
        finger.emptyDatabase();
        displayMessage("Fingerprint", "Data Cleared", 2000);
        resetPasscodeEntry();
    }
}

// Setup and main loop
void setup() {
    Serial.begin(115200);

    // Initialize LCD
    lcd.init();
    lcd.backlight();
    displayMessage("Initializing...", "Please wait");

    // Setup servo
    lockServo.attach(SERVO_PIN);     // Min/Max pulse width
    delay(3000);                     // Allow time for servo to initialize
    lockServo.write(LOCK_POSITION);  // Start in locked positio

    // Setup I/O and interfaces
    pinMode(MOVEMENT_PIN, INPUT);
    resetPasscodeEntry();
    lastKeyPressTime = millis();

    // Initialize fingerprint sensor
    finger.begin(57600);
    if (finger.verifyPassword()) {
        Serial.println("Fingerprint sensor connected");
        finger.getTemplateCount();
        Serial.println("Found " + String(finger.templateCount) + " templates");
    } else {
        Serial.println("Fingerprint sensor not found!");
    }

    // Load PIN from preferences
    unlockPin = loadPin();
    Serial.println("Current PIN: " + unlockPin);

    // Initialize Blynk
    BlynkEdgent.begin();

    // Create input handling task
    xTaskCreatePinnedToCore(inputTask, "InputTask", 4096, NULL, 1,
                            &inputTaskHandle, 0);
}

void loop() {
    BlynkEdgent.run();
    delay(1000);
}