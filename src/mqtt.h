#ifndef PITROVE_MQTT_H
#define PITROVE_MQTT_H

#include <string>

void start_mqtt_client();
void mqtt_publish(const std::string& subtopic, const std::string& payload, bool retain = false);
void publish_ha_discovery();

#endif // PITROVE_MQTT_H
