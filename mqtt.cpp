#include "mosquitto.h"
#include "mqtt.h"


mosquitto* mosq = nullptr;

void handlePitchMsg(
	MQTTState* mqttState,
	const mosquitto_message* msg)
{
	if (msg->payloadlen) {
		u32 val = strtoul((char*)msg->payload, nullptr, 10);
		mqttState->rawPitch = val;
		u64 since = timer.now_nsec - mqttState->pitch_nsec;
		mqttState->pitch_nsec = timer.now_nsec; // need sync? assignment of u64 is not atomic
		printf("handlePitchMsg mid=%d, val=%u, at %llu, %llu since last\n", msg->mid, mqttState->rawPitch, mqttState->pitch_nsec, since);
	}
}

void handleBankMsg(
	MQTTState* mqttState,
	const mosquitto_message* msg)
{
	if (msg->payloadlen) {
		u32 val = strtoul((char*)msg->payload, nullptr, 10);
		mqttState->rawBank = val;
		u64 since = timer.now_nsec - mqttState->bank_nsec;
		mqttState->bank_nsec = timer.now_nsec;
		printf("handleBankMsg mid=%d, val=%u, at %llu, %llu since last\n", msg->mid, mqttState->rawBank, mqttState->bank_nsec, since);
	}
}

void handleGoodbyeMsg(
	MQTTState* mqttState,
	const mosquitto_message* msg)
{
	MQTTState& st = *mqttState;
	st.pitch_nsec = st.bank_nsec = st.turn_nsec = 0;
}


typedef void MsgCallback(
	MQTTState* mqttState,
	const mosquitto_message* msg);

// subscriptions
struct MQTTSubscription {
	const char*		topic;
	int				qos;
	MsgCallback*	callback;
	int				mid;
};

MQTTSubscription subs[] = {
	{ "dcs-bios/output/adi/adi_pitch",        0, handlePitchMsg },
	{ "dcs-bios/output/adi/adi_bank",         0, handleBankMsg },
	{ "dcs-bios/output/adi/adi_turn",         0, nullptr },
	{ "dcs-bios/output/adi/adi_slip",         0, nullptr },
	{ "dcs-bios/output/adi/adi_gs",           0, nullptr },
	{ "dcs-bios/output/adi/adi_steer_bank",   0, nullptr },
	{ "dcs-bios/output/adi/adi_steer_pitch",  0, nullptr },
	{ "dcs-bios/output/adi/adi_pitch_trim",   1, nullptr },
	{ "dcs-bios/output/adi/adi_attwarn_flag", 1, nullptr },
	{ "dcs-bios/output/adi/adi_crswarn_flag", 1, nullptr },
	{ "dcs-bios/output/adi/adi_gswarn_flag",  1, nullptr },
	{ "dcs-bios/output/light_system_control_panel/lcp_flight_inst", 0, nullptr },
	{ "dcs-bios/output/metadata/_acft_name",  1, nullptr },
	{ "dcs-bios/goodbye",                     1, handleGoodbyeMsg }
};


void onConnect(
	mosquitto* mosq,
	void* userdata,
	int result)
{
	if (!result) {
		/* Subscribe to broker information topics on successful connect. */
		mosquitto_subscribe(mosq, NULL, "$SYS/#", 2);
	}
	else {
		fprintf(stderr, "Connect failed\n");
	}
}


void onSubscribe(
	mosquitto* mosq,
	void* userdata,
	int mid,
	int qosCount,
	const int* grantedQos)
{
	const char* topic = nullptr;
	for(uint32_t s = 0;
		s < countof(subs);
		++s)
	{
		if (subs[s].mid == mid) {
			topic = subs[s].topic;
			break;
		}
	}
	if (topic == nullptr) {
		topic = "sys";
	}

	printf("Subscribed: mid=%d: qos=%d", mid, grantedQos[0]);
	for(int i = 1;
		i < qosCount;
		++i)
	{
		printf(" %d", grantedQos[i]);
	}
	printf(": topic=%s\n", topic);
}


void onMessage(
	mosquitto* mosq,
	void* userdata,
	const mosquitto_message* message)
{
	for(u32 s = 0;
		s < countof(subs);
		++s)
	{
		bool match = false;
		int result = mosquitto_topic_matches_sub(subs[s].topic, message->topic, &match);
		if (result == MOSQ_ERR_SUCCESS && match) {
			if (subs[s].callback) {
				subs[s].callback((MQTTState*)userdata, message);
			}
			break;
		}
	}

	if (message->payloadlen) {
		printf("%s %s\n", message->topic, (char*)message->payload);
	}
	else {
		printf("%s (null)\n", message->topic);
	}
	fflush(stdout);
}


bool initMQTT(
	MQTTState* mqttState)
{
	mosquitto_lib_init();

	mosq = mosquitto_new(
		MQTT_CLIENT_ID, // client_id
		true,           // clean_session
		mqttState);     // userdata
	if (!mosq) {
		fprintf(stderr, "Create client error: Out of memory or invalid input\n");
		return false;
	}

	//mosquitto_log_callback_set(mosq, onLog);
	mosquitto_connect_callback_set(mosq, onConnect);
	mosquitto_message_callback_set(mosq, onMessage);
	mosquitto_subscribe_callback_set(mosq, onSubscribe);

	int rc = mosquitto_connect(
		mosq,
		MQTT_HOST,
		MQTT_PORT,
		60);  // keepalive

	if (rc != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "Unable to connect: %s\n", mosquitto_strerror(rc));
		return false;
	}

	for(u32 s = 0; s < countof(subs); ++s) {
		rc = mosquitto_subscribe(
			mosq,
			&subs[s].mid,
			subs[s].topic,
			subs[s].qos);

		if (rc != MOSQ_ERR_SUCCESS) {
			printf("Subscribe error: %s: %s\n", subs[s].topic, mosquitto_strerror(rc));
			return false;
		}
	}

	mosquitto_reconnect_delay_set(
		mosq,
		2,     // starting delay
		64,    // max delay
		true); // exponential backoff

	mosquitto_loop_start(mosq);

	printf("MQTT running...\n");

	return true;
}


void cleanupMQTT()
{
	mosquitto_disconnect(mosq);
	mosquitto_loop_stop(mosq, true);

	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();

	printf("MQTT cleanup done\n");
}
