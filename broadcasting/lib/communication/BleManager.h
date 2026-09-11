#pragma once
#include "Config.h"
#include "DataTypes.h"
#include <bluefruit.h>

void setupBLE();
void updateAdvertising(float bodyTemp, float humidity,
                       uint32_t timestamp, uint16_t seq, uint8_t flags);
