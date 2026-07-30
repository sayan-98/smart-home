// ---------------------------------------------------------------------------
//  MqttService.h - the cloud realtime bus.
//
//  Built on the ESP-IDF native client (esp_mqtt), NOT PubSubClient. The spec
//  asks for "MQTT with QoS" and PubSubClient can only PUBLISH at QoS 0 - it
//  would have quietly downgraded every state report. esp_mqtt gives real
//  QoS 1 both ways, TLS with the bundled Mozilla root store, LWT, an outbox
//  that survives brief disconnects, and its own task.
//
//  Topic tree (see docs/MQTT.md):
//    smarthome/<homeId>/<uuid>/status            retained, LWT: online|offline
//    smarthome/<homeId>/<uuid>/register          published once per connect
//    smarthome/<homeId>/<uuid>/state             retained full snapshot
//    smarthome/<homeId>/<uuid>/relay/<ch>/state  retained per channel
//    smarthome/<homeId>/<uuid>/relay/<ch>/set    subscribed
//    smarthome/<homeId>/<uuid>/relay/all/set     subscribed
//    smarthome/<homeId>/<uuid>/cmd               subscribed
//    smarthome/<homeId>/<uuid>/diag              heartbeat, every 30 s
//
//  Retained state topics are what let a phone that has been closed for three
//  days open to the truth instantly, with no round trip to the device.
//
//  NOTE ON FREE HOSTING: Render's free tier cannot host a broker (no raw TCP,
//  and it sleeps). Point mqttHost at HiveMQ Cloud Serverless free instead -
//  which is TLS-only, so mqttTls must be true for any cloud deployment.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sh {

class MqttService {
 public:
  static void begin();

  /// Connects (or reconnects) using the current configuration. Called when
  /// Wi-Fi comes up and after the device is claimed.
  static void connect();
  static void disconnect();

  static bool isConnected();
  static bool isEnabled();
  static uint32_t reconnectCount();
  static uint32_t publishCount();

  /// Publishes the complete relay snapshot plus every per-channel retained
  /// topic. Called on connect and after any bulk change, so a reconnecting
  /// device re-asserts the truth rather than waiting for the next event.
  static void publishFullState();

  static void publishDiagnostics();

  /// Base topic for this device, e.g. "smarthome/<homeId>/<uuid>".
  static const char* baseTopic();
};

}  // namespace sh
