#define BLYNK_TEMPLATE_ID "TMPL608a18IeD"
#define BLYNK_TEMPLATE_NAME "EasyLock SmartDoor"
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
const int LOCK_POSITION = 90;
const int UNLOCK_POSITION = 0;
const unsigned long UNLOCK_DURATION = 15000;
const unsigned long LOCKOUT_DURATION = 60000;

// Keypad timeout
const unsigned long KEYPAD_TIMEOUT = 30000;

// PIN constants
const uint8_t PASSCODE_LENGTH = 6;

// System state
Keypad keypad =
    Keypad(makeKeymap(keyMap), (byte *)rowPins, (byte *)colPins, 4, 4);
TaskHandle_t inputTaskHandle = NULL;

String currentPasscode = "";

int pinFailedAttempts = 0;
int fingerFailedAttempts = 0;
const int FINGER_ENROLL_ATTEMPTS = 3;
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
        return true;
    } else {
        Serial.println("Failed to save PIN to preferences");
        return false;
    }
}

void sendBlynkEvent(String eventName, String eventDescription) {
    if (Blynk.connected()) {
        Blynk.logEvent(eventName, eventDescription);
        Serial.println("Event sent: " + String(eventName));
    } else {
        Serial.println("Blynk offline, can't send event: " + String(eventName));
    }
}

void resetPasscodeEntry() {
    currentPasscode = "";
    displayUpdate(0, 0, "Enter Passcode:", true);
    displayUpdate(5, 1, "______");
}

bool handleKeypadInput() {
    if (currentPasscode.length() > 0 &&
        millis() - lastKeyPressTime >= KEYPAD_TIMEOUT) {
        displayMessage("Timeout", "Input cleared", 1500);
        resetPasscodeEntry();
        return false;
    }

    char key = keypad.getKey();
    if (!key) return false;
    lastKeyPressTime = millis();

    if (isdigit(key) && currentPasscode.length() < PASSCODE_LENGTH) {
        // Add digit to the string
        currentPasscode += key;
        displayUpdate(5 + currentPasscode.length() - 1, 1, String(key));

        if (currentPasscode.length() == PASSCODE_LENGTH) {
            displayUpdate(0, 0, "Press # to verify", false);
        }
        return false;
    }

    // Handle backspace (*)
    if (key == '*' && currentPasscode.length() > 0) {
        // Remove last character from string
        displayUpdate(5 + currentPasscode.length() - 1, 1, "_");
        currentPasscode.remove(currentPasscode.length() - 1);
        return false;
    }

    // Handle enter key (#)
    if (key == '#' && currentPasscode.length() == PASSCODE_LENGTH) {
        // Direct string comparison
        if (currentPasscode == loadPin()) {
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

bool isFingerprintExist(int id) {
    uint8_t p = finger.loadModel(id);
    return p == FINGERPRINT_OK;
}

uint8_t findAvailableFingerID() {
    finger.getTemplateCount();

    for (uint8_t id = 1; id < 128; id++) {
        uint8_t p = finger.loadModel(id);
        Serial.println("ID: " + String(id) + " - " + String(p));
        if (!isFingerprintExist(id)) {
            return id;
        }
    }

    return 0;
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

    if (id == 0) {
        displayMessage("No free slots", "Clear database first", 2000);
        return false;
    }

    displayMessage("Enrolling ID #" + String(id), "Place finger");
    Serial.println("Waiting for valid finger to enroll as #" + String(id));

    // First image capture
    attemptCount = 0;
    while (p != FINGERPRINT_OK && attemptCount < FINGER_ENROLL_ATTEMPTS) {
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

        if (attemptCount >= FINGER_ENROLL_ATTEMPTS) {
            displayMessage("Too many errors", "Registration canceled", 2000);
            return false;
        }
    }

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

    p = finger.fingerFastSearch();
    if (p == FINGERPRINT_OK) {
        displayMessage("Already exists", "ID #" + String(finger.fingerID),
                       2000);
        Serial.println("Fingerprint already exists with ID #" +
                       String(finger.fingerID));
        return false;
    }
    Serial.println("No duplicate found, continuing enrollment");

    displayMessage("Remove finger", "from sensor");
    Serial.println("Remove finger");
    delay(1000);
    p = 0;

    attemptCount = 0;
    while (p != FINGERPRINT_NOFINGER && attemptCount < FINGER_ENROLL_ATTEMPTS) {
        p = finger.getImage();
        if (p != FINGERPRINT_NOFINGER) {
            delay(1000);
            attemptCount++;
        }

        if (attemptCount >= FINGER_ENROLL_ATTEMPTS) {
            displayMessage("Finger not removed", "Registration canceled", 2000);
            return false;
        }
    }

    displayMessage("Place same", "finger again");
    Serial.println("Place same finger again for ID #" + String(id));
    p = -1;

    attemptCount = 0;
    while (p != FINGERPRINT_OK && attemptCount < FINGER_ENROLL_ATTEMPTS) {
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

        if (attemptCount >= FINGER_ENROLL_ATTEMPTS) {
            displayMessage("Too many errors", "Registration canceled", 2000);
            return false;
        }
    }

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

bool handleFingerprint() {
    // Early return for conditions that prevent fingerprint processing
    if (isRegistering.load() || !isLocked || isLockoutActive()) {
        return false;
    }

    // First check if there's a finger
    int fingerID = getFingerprintIDez();
    if (fingerID == -1) {
        return false;  // No finger detected
    }

    // Show message once and wait for finger removal
    displayMessage("Remove your", "finger to process");
    while (finger.getImage() != FINGERPRINT_NOFINGER) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Process result
    if (fingerID > 0) {
        // Success - valid fingerprint
        fingerFailedAttempts = 0;
        displayMessage("Access Granted!", "Door Unlocked", 2000);
        unlockTemporarily();
        sendBlynkEvent("access_granted", "Access granted via fingerprint");
        return true;
    } else if (fingerID == -2) {
        // Failed match
        fingerFailedAttempts++;
        lastFingerFailTime = millis();

        // Handle too many failed attempts
        if (fingerFailedAttempts >= 5) {
            sendBlynkEvent("send_alarm", "Access denied, too many attempts");
            displayMessage("Too Many Attempts", "Locking out", 2000);
            lockoutUntil = millis() + LOCKOUT_DURATION;
            fingerFailedAttempts = 0;
        } else {
            sendBlynkEvent("access_denied", "Access denied via fingerprint");
            displayMessage("Access Denied!",
                           String(5 - fingerFailedAttempts) + " attempts left",
                           2000);
        }
        resetPasscodeEntry();
    }

    return false;
}

void blynkVirtualWrite(const int pin, String value) {
    if (WiFi.status() == WL_CONNECTED && Blynk.connected()) {
        Blynk.virtualWrite(pin, value);
    } else {
        Serial.println("Blynk offline, can't send event: " + String(pin));
    }
}

const unsigned long MOTION_DEBOUNCE = 80;

// Main input task
void inputTask(void *parameter) {
    unsigned long lastMotionTime = 0;

    int fingerScanCounter = 0;
    int pinStateCurrent = digitalRead(MOVEMENT_PIN);
    int pinStatePrevious = pinStateCurrent;
    long lastDisplayedTime = -1;
    bool wasLocked = false;

    for (;;) {
        unsigned long currentTime = millis();
        if (isLockoutActive()) {
            wasLocked = true;
            unsigned long remainingSecs = (lockoutUntil - currentTime) / 1000;
            displayMessage("System Locked",
                           String(remainingSecs) + "s remaining");

            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        if (wasLocked) {
            wasLocked = false;
            displayMessage("Lockout Ended", "System Available", 2000);
            resetPasscodeEntry();
        }

        if (autoLockPending) {
            pinStatePrevious = pinStateCurrent;
            pinStateCurrent = digitalRead(MOVEMENT_PIN);

            if (pinStatePrevious == LOW && pinStateCurrent == HIGH &&
                (currentTime - lastMotionTime > MOTION_DEBOUNCE)) {
                lastMotionTime = currentTime;
                Serial.println("Motion detected!");
                autoLockTime = currentTime + UNLOCK_DURATION;
            }

            if (currentTime >= autoLockTime) {
                setLockPosition(true);
                autoLockPending = false;
                displayMessage("Door Locked", "Auto-lock complete", 1000);
                resetPasscodeEntry();
                lastDisplayedTime = -1;
            } else {
                long remainingTime = (autoLockTime - currentTime) / 1000;
                if (remainingTime != lastDisplayedTime && remainingTime >= 0) {
                    lastDisplayedTime = remainingTime;
                    displayMessage("Auto-Lock in:",
                                   String(remainingTime) + "s");
                }
            }
        }

        if (pinFailedAttempts > 0 &&
            (millis() - lastPinFailTime) > ATTEMPT_RESET_TIME) {
            Serial.println("PIN failed attempts reset after 2 minutes");
            pinFailedAttempts = 0;
        }

        if (fingerFailedAttempts > 0 &&
            (millis() - lastFingerFailTime) > ATTEMPT_RESET_TIME) {
            Serial.println("Fingerprint failed attempts reset after 2 minutes");
            fingerFailedAttempts = 0;
        }

        if (isLocked) {
            bool keypadSuccess = handleKeypadInput();
            if (keypadSuccess || isLockoutActive()) {
                vTaskDelay(50 / portTICK_PERIOD_MS);
                continue;
            }

            fingerScanCounter++;
            if (fingerScanCounter >= 5) {
                fingerScanCounter = 0;
                handleFingerprint();
            }
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

BLYNK_WRITE(V0) {
    if (param.asInt()) {
        displayMessage("Door Unlocked", "Blynk Command");
        lockoutUntil = 0;
        unlockTemporarily();
        resetPasscodeEntry();
    }
}

const unsigned long PIN_CHANGE_COOLDOWN = 15000;
unsigned long lastPinChangeAttempt = 0;

BLYNK_WRITE(V1) {
    String newPin = param.asStr();
    unsigned long currentTime = millis();

    if (lastPinChangeAttempt > 0 &&
        currentTime - lastPinChangeAttempt < PIN_CHANGE_COOLDOWN) {
        int remainingSeconds =
            (PIN_CHANGE_COOLDOWN - (currentTime - lastPinChangeAttempt)) / 1000;

        String message = "Please wait " + String(remainingSeconds) +
                         " seconds before changing PIN again";
        blynkVirtualWrite(V3, message);
        displayMessage("PIN Change Limit",
                       "Wait " + String(remainingSeconds) + "s", 2000);
        return;
    }

    lastPinChangeAttempt = currentTime;

    if (!isValidPin(newPin)) {
        blynkVirtualWrite(V3, "Invalid PIN format, please try again");
        return;
    }

    if (savePin(newPin)) {
        displayMessage("PIN Changed", "Successfully", 4000);
        blynkVirtualWrite(V3, "PIN changed successfully");
    } else {
        displayMessage("PIN Change", "Failed", 3000);
        blynkVirtualWrite(V3, "PIN change failed");
    }

    resetPasscodeEntry();
}

const unsigned long FINGERPRINT_REGISTER_COOLDOWN = 60000;  // 60 seconds
unsigned long lastRegistrationAttempt = 0;

BLYNK_WRITE(V2) {
    if (param.asInt()) {
        if (isRegistering) {
            blynkVirtualWrite(V4, "Registration already in progress");
            return;
        }

        unsigned long currentTime = millis();
        if (lastRegistrationAttempt > 0 &&
            currentTime - lastRegistrationAttempt <
                FINGERPRINT_REGISTER_COOLDOWN) {
            int remainingSeconds = (FINGERPRINT_REGISTER_COOLDOWN -
                                    (currentTime - lastRegistrationAttempt)) /
                                   1000;

            String message = "Please wait " + String(remainingSeconds) +
                             " seconds before registering again";

            blynkVirtualWrite(V4, message);
            return;
        }

        isRegistering = true;
        bool success = getFingerprintEnroll();
        isRegistering = false;

        // Apply cooldown regardless of success or failure
        lastRegistrationAttempt = millis();

        if (success) {
            displayMessage("Registration", "Successful", 2000);
            blynkVirtualWrite(V4, "Fingerprint registered successfully");

            finger.getTemplateCount();
            blynkVirtualWrite(
                V5, String(finger.templateCount) + " fingerprints now stored");
        } else {
            displayMessage("Registration", "Failed", 2000);
            blynkVirtualWrite(
                V4, "Fingerprint registration failed - try again in 60s");
        }
        resetPasscodeEntry();
    }
}

BLYNK_WRITE(V6) {
    int id = param.asInt();
    if (id > 0) {
        if (!isFingerprintExist(id)) {
            displayMessage("ID #" + String(id), "not found", 2000);
            blynkVirtualWrite(
                V5, "Fingerprint ID #" + String(id) + " not found in database");
            return;
        }

        uint8_t result = finger.deleteModel(id);
        if (result == FINGERPRINT_OK) {
            displayMessage("Fingerprint", "Deleted", 2000);
            finger.getTemplateCount();

            String detailedMsg =
                "Fingerprint ID #" + String(id) + " successfully deleted";
            detailedMsg +=
                " (" + String(finger.templateCount) + " fingerprints stored)";

            blynkVirtualWrite(V5, detailedMsg);
            Serial.println(detailedMsg);
        } else {
            displayMessage("Delete Failed", "Try again", 2000);
            String errorMsg = "Failed to delete ID #" + String(id) + ": ";

            switch (result) {
                case FINGERPRINT_PACKETRECIEVEERR:
                    errorMsg += "Communication error with sensor";
                    break;
                case FINGERPRINT_BADLOCATION:
                    errorMsg += "ID does not exist in database";
                    break;
                case FINGERPRINT_FLASHERR:
                    errorMsg += "Flash memory write error";
                    break;
                default:
                    errorMsg += "Error code " + String(result);
                    break;
            }

            // Send detailed error to Blynk app
            blynkVirtualWrite(V5, errorMsg);
            Serial.println(errorMsg);
        }
    } else {
        // Invalid ID handling
        displayMessage("Invalid ID", "Try again", 2000);
        blynkVirtualWrite(V5, "Invalid fingerprint ID: must be greater than 0");
    }
    resetPasscodeEntry();
}

BLYNK_WRITE(V7) {
    if (param.asInt()) {
        if (isLocked) {
            blynkVirtualWrite(V8, "Door already locked");
        } else {
            autoLockTime = 0;
            blynkVirtualWrite(V8, "Door locked successfully");
        }
    }
}

void setup() {
    Serial.begin(115200);

    // Initialize LCD
    lcd.init();
    lcd.backlight();
    displayMessage("Initializing...", "Please wait");

    // Setup servo
    lockServo.attach(SERVO_PIN);
    delay(1000);
    lockServo.write(LOCK_POSITION);

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
    Serial.println("Current PIN: " + loadPin());

    // Initialize Blynk
    BlynkEdgent.begin();

    // Create input handling task
    xTaskCreatePinnedToCore(inputTask, "InputTask", 4096, NULL, 1,
                            &inputTaskHandle, 0);

    currentPasscode.reserve(PASSCODE_LENGTH + 1);
}

void handleReset() {
    Preferences prefs;
    if (prefs.begin("smartlock", false)) {
        if (prefs.getBool("flag_reset", false)) {
            prefs.remove("pin");
            Serial.println("PIN removed from preferences");
            displayMessage("PIN Reset", "Done", 2000);

            finger.emptyDatabase();
            Serial.println("Fingerprint database cleared");
            displayMessage("Fingerprint DB", "Cleared", 2000);

            prefs.putBool("flag_reset", false);
            Serial.println("Reset to factory defaults");

            displayMessage("Factory Reset", "Done", 4000);
            resetPasscodeEntry();
        }

        prefs.end();
    }
}

void loop() {
    handleReset();

    static unsigned long lastMemReport = 0;
    static unsigned long lastHeapCheck = 0;
    unsigned long now = millis();

    // Debug memory - written by Claude AI
    if (now - lastMemReport > 30000) {
        Serial.println("---------------------------------");
        Serial.println("Memory report:");
        Serial.printf("Free heap: %d bytes, Largest block: %d bytes\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        lastMemReport = now;
        Serial.printf("Task Count: %u\n", uxTaskGetNumberOfTasks());

        Serial.printf("Input Task Stack HWM: %u\n",
                      uxTaskGetStackHighWaterMark(inputTaskHandle));
        Serial.println("---------------------------------");
    }

    if (now - lastHeapCheck > 10000) {  // Check every 10 seconds
        lastHeapCheck = now;
        if (ESP.getFreeHeap() < 10000) {  // Critical threshold
            Serial.println("WARNING: Low memory detected");
            // Optional: Take recovery action or restart
            // ESP.restart();
        }
    }

    // End debug memory

    BlynkEdgent.run();
    delay(1000);
}