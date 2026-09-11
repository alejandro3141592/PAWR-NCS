#include "SensorManager.h"
#include "DataTypes.h"
#include <Arduino.h>

bool SensorManager::begin() {
    digitalWrite(LED_GREEN, LOW);  delay(300);
    digitalWrite(LED_GREEN, HIGH);

    max30205Ok = max30205.begin();
    if (!max30205Ok) Serial.println("[WARN] MAX30205 init failed");

    sht41Ok = sht41.begin();
    if (!sht41Ok) Serial.println("[WARN] SHT41 init failed");

    digitalWrite(LED_GREEN, LOW);  delay(300);
    digitalWrite(LED_GREEN, HIGH);
    return true;
}

void SensorManager::update() {
    if (max30205Ok) max30205.update();
    if (sht41Ok)    sht41.update();
}

void SensorManager::print() {
    Serial.println("---- Sensor Readings ----");
    Serial.print("  Body Temp (MAX30205): ");
    if (max30205Ok) { Serial.print(max30205.getTemperature(), 2); Serial.println(" C"); }
    else              Serial.println("N/A");

    Serial.print("  Ambient Temp (SHT41): ");
    if (sht41Ok) { Serial.print(sht41.getTemperature(), 2); Serial.println(" C"); }
    else           Serial.println("N/A");

    Serial.print("  Humidity (SHT41):     ");
    if (sht41Ok) { Serial.print(sht41.getHumidity(), 1); Serial.println(" %"); }
    else           Serial.println("N/A");

    Serial.println("--------------------------");
}

float SensorManager::getBodyTemperature()    const { return max30205Ok ? max30205.getTemperature() : NAN; }
float SensorManager::getAmbientTemperature() const { return sht41Ok    ? sht41.getTemperature()    : NAN; }
float SensorManager::getHumidity()           const { return sht41Ok    ? sht41.getHumidity()       : NAN; }
