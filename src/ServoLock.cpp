#include "ServoLock.h"

ServoLock::ServoLock(int pin, int lockPos, int unlockPos, unsigned long unlockTime)
    : servoPin(pin),
      lockPosition(lockPos),
      unlockPosition(unlockPos),
      isLocked(true),
      unlockDuration(unlockTime)
{
}

void ServoLock::init()
{
    ESP32PWM::allocateTimer(0); // Use first timer
    servo.attach(servoPin);

    lock(); // Start in locked position
}

void ServoLock::lock()
{
    servo.write(lockPosition);
    isLocked = true;
    Serial.println("Door locked");
}

void ServoLock::unlock()
{
    servo.write(unlockPosition);
    isLocked = false;
    Serial.println("Door unlocked");
}

void ServoLock::unlockTemporarily()
{
    unlock();
    delay(unlockDuration); // Keep unlocked for specified duration
    lock();
}

bool ServoLock::getStatus()
{
    return isLocked;
}

void ServoLock::setPositions(int lockPos, int unlockPos)
{
    lockPosition = lockPos;
    unlockPosition = unlockPos;
    // Apply the current state with the new positions
    if (isLocked)
    {
        lock();
    }
    else
    {
        unlock();
    }
}

void ServoLock::setUnlockDuration(unsigned long duration)
{
    unlockDuration = duration;
}