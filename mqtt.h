#ifndef _MQTT_H
#define _MQTT_H

#include "utility/types.h"

#define MQTT_HOST      "192.168.0.174"
#define MQTT_PORT      1883
#define MQTT_CLIENT_ID "ADI"

struct MQTTState {
	u32		rawPitch;
	u32		rawBank;
	u32		rawTurn;

	// last update timestamps
	u64		pitch_nsec;
	u64		bank_nsec;
	u64		turn_nsec;
};

bool initMQTT(
	MQTTState* mqttState);

void cleanupMQTT();

#endif