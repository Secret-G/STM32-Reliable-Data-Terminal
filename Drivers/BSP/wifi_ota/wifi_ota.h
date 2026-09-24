#ifndef WIFI_OTA_H
#define WIFI_OTA_H

#include "device_protocol.h"
#include "boot_protocol.h"

#define WIFI_OTA_START_PAYLOAD_SIZE       10U
#define WIFI_OTA_END_PAYLOAD_SIZE          4U
#define WIFI_OTA_RESPONSE_PAYLOAD_SIZE     9U

/* OTA_DATA的sequence复用外层帧序号，整个payload都可用于固件数据。 */
#define WIFI_OTA_DATA_MAX_SIZE DEVICE_PROTOCOL_MAX_PAYLOAD_SIZE

#if WIFI_OTA_DATA_MAX_SIZE > BOOT_DATA_MAX_DATA_SIZE
#error "WIFI_OTA_DATA_MAX_SIZE exceeds AppUpdate data capacity"
#endif

#define WIFI_OTA_ENTER_TIMEOUT_MS       10000U
#define WIFI_OTA_SESSION_TIMEOUT_MS     30000U
#define WIFI_OTA_PENDING_RESET_TIMEOUT_MS 10000U
#define WIFI_OTA_RESET_DELAY_MS           500U

typedef uint8_t (*WifiOta_SendFrame_t)(const uint8_t *data, uint16_t length);

void WifiOta_Init(WifiOta_SendFrame_t send_frame);
uint8_t WifiOta_HandleFrame(const DeviceProtocol_Frame_t *frame,
                            uint8_t business_in_flight);
void WifiOta_Process(uint8_t business_in_flight);
void WifiOta_OnTransportDisconnected(void);
uint8_t WifiOta_IsActive(void);

#endif
