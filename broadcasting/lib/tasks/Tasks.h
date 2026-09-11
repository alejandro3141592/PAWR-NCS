#pragma once

#include "Config.h"
#include "SharedState.h"
#include "BleManager.h"
#include "DataTypes.h"
#include "SensorManager/SensorManager.h"




void sensorTask(void *pvParameters);
void communicationTask(void *pvParameters);