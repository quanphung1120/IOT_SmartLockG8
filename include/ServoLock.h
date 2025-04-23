#ifndef SERVO_LOCK_H
#define SERVO_LOCK_H

#include <Arduino.h>
#include <ESP32Servo.h> // Changed to ESP32Servo.h

class ServoLock
{
private:
    Servo servo;
    int servoPin;
    int lockPosition;
    int unlockPosition;
    bool isLocked;
    unsigned long unlockDuration; // Duration to keep door unlocked in ms

public:
    ServoLock(int pin, int lockPos = 0, int unlockPos = 90, unsigned long unlockTime = 5000);

    void init();
    void lock();
    void unlock();
    void unlockTemporarily(); // Unlocks for a set duration, then locks again
    bool getStatus();         // Returns true if locked, false if unlocked
    void setPositions(int lockPos, int unlockPos);
    void setUnlockDuration(unsigned long duration);
};

#endif // SERVO_LOCK_H