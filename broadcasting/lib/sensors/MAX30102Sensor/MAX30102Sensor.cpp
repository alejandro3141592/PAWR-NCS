#include "MAX30102Sensor.h"
#include "spo2_algorithm.h"
#include <Arduino.h>

bool MAX30102Sensor::begin() {
    if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("[WARN] MAX30102 not found");
        return false;
    }

    particleSensor.setup(
        60,    // LED brightness
        4,     // sample average
        2,     // LED mode (Red + IR)
        100,   // sample rate
        411,   // pulse width
        4096   // ADC range
    );

    return true;
}

void MAX30102Sensor::update() {
    while (particleSensor.available()) {
        irBuffer[bufferIndex]  = particleSensor.getIR();
        redBuffer[bufferIndex] = particleSensor.getRed();
        particleSensor.nextSample();

        bufferIndex++;
        if (bufferIndex >= BUFFER_SIZE) {
            bufferIndex  = 0;
            bufferFilled = true;
        }
    }

    if (!bufferFilled) return;

    int32_t spo2Int;  int8_t spo2Valid;
    int32_t hrInt;    int8_t hrValid;

    maxim_heart_rate_and_oxygen_saturation(
        irBuffer, BUFFER_SIZE, redBuffer,
        &spo2Int, &spo2Valid, &hrInt, &hrValid
    );

    if (hrValid)   heartRate = (float)hrInt;
    if (spo2Valid) spo2      = (float)spo2Int;
}

float MAX30102Sensor::getHeartRate() const { return heartRate; }
float MAX30102Sensor::getSpO2()      const { return spo2; }
