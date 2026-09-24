#ifndef APP_UPDATE_H
#define APP_UPDATE_H

#include "boot_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    APP_UPDATE_IDLE = 0,
    APP_UPDATE_RECEIVING,
    APP_UPDATE_READY,
    APP_UPDATE_ERROR
} AppUpdate_StateTypeDef;

void AppUpdate_Init(void);
void AppUpdate_Abort(void);

Boot_ResultTypeDef AppUpdate_HandleStart(
    const Boot_StartInfoTypeDef *start_info,
    uint32_t *response_value);

Boot_ResultTypeDef AppUpdate_HandleData(
    const Boot_DataInfoTypeDef *data_info,
    uint32_t *response_value);

Boot_ResultTypeDef AppUpdate_HandleEnd(
    uint32_t packet_count,
    uint32_t *response_value);

AppUpdate_StateTypeDef AppUpdate_GetState(void);
		

#ifdef __cplusplus
}
#endif

#endif /* APP_UPDATE_H */
