/*
 * Copyright 2019-2022 u-blox
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** @file
 * @brief Functions associated with a GNSS device.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h"  // For a customer's configuration override
#endif

#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "stdlib.h"    // malloc()/free()
#include "string.h"    // for memset()

#include "u_cfg_os_platform_specific.h" // For U_CFG_OS_CLIB_LEAKS

#include "u_error_common.h"

#include "u_port_uart.h"
#include "u_port_i2c.h"

#include "u_device.h"
#include "u_device_shared.h"

#include "u_at_client.h"

#include "u_cell_module_type.h"
#include "u_cell.h"                 // For uCellAtClientHandleGet()
#include "u_cell_loc.h"             // For uCellLocSetPinGnssPwr()/uCellLocSetPinGnssDataReady()

#include "u_short_range_module_type.h"
#include "u_short_range.h"          // For uShortRangeAtClientHandleGet()

#include "u_gnss_module_type.h"
#include "u_gnss_type.h"
#include "u_gnss.h"
#include "u_gnss_pwr.h"

#include "u_device_shared_gnss.h"
#include "u_device_private_gnss.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC VARIABLES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Do all the leg-work to remove a GNSS device.
static int32_t removeDevice(uDeviceHandle_t devHandle, bool powerOff)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    uDeviceGnssInstance_t *pContext = (uDeviceGnssInstance_t *) U_DEVICE_INSTANCE(devHandle)->pContext;

    if (pContext != NULL) {
        errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
        if (powerOff) {
            errorCode = uGnssPwrOff(devHandle);
            if (errorCode == 0) {
                // This will destroy the instance
                uGnssRemove(devHandle);
                free(pContext);
            }
        } else {
            free(pContext);
        }
    }

    return errorCode;
}

// Do all the leg-work to add a GNSS device.
static int32_t addDevice(int32_t transportHandle,
                         uDeviceTransportType_t transportType,
                         const uDeviceCfgGnss_t *pCfgGnss,
                         uDeviceHandle_t *pDeviceHandle)
{
    int32_t errorCodeOrHandle = (int32_t) U_ERROR_COMMON_NO_MEMORY;
    uGnssTransportHandle_t gnssTransportHandle;
    uGnssTransportType_t gnssTransportType = U_GNSS_TRANSPORT_UBX_UART;
    uDeviceGnssInstance_t *pContext;

    // Populate gnssTransportHandle/gnssTransportType
    if (transportType == U_DEVICE_TRANSPORT_TYPE_I2C) {
        gnssTransportHandle.i2c = transportHandle;
        gnssTransportType = U_GNSS_TRANSPORT_UBX_I2C;
        if (pCfgGnss->includeNmea) {
            gnssTransportType = U_GNSS_TRANSPORT_NMEA_I2C;
        }
    } else {
        gnssTransportHandle.uart = transportHandle;
        if (pCfgGnss->includeNmea) {
            gnssTransportType = U_GNSS_TRANSPORT_NMEA_UART;
        }
    }

    pContext = (uDeviceGnssInstance_t *) malloc(sizeof(uDeviceGnssInstance_t));
    if (pContext != NULL) {
        pContext->transportHandle = transportHandle;
        pContext->transportType = transportType;
        // Add the GNSS instance, which actually creates pDeviceHandle
        errorCodeOrHandle = uGnssAdd((uGnssModuleType_t) pCfgGnss->moduleType,
                                     gnssTransportType, gnssTransportHandle,
                                     pCfgGnss->pinEnablePower, false,
                                     pDeviceHandle);
        if (errorCodeOrHandle == 0) {
#if !U_CFG_OS_CLIB_LEAKS
            // Set printing of commands sent to the GNSS chip,
            // which can be useful while debugging, but
            // only if the C library doesn't leak.
            uGnssSetUbxMessagePrint(*pDeviceHandle, true);
#endif
            // Attach the context
            U_DEVICE_INSTANCE(*pDeviceHandle)->pContext = pContext;
            // Power on the GNSS chip
            errorCodeOrHandle = uGnssPwrOn(*pDeviceHandle);
            if (errorCodeOrHandle != 0) {
                // If we failed to power on, clean up
                removeDevice(*pDeviceHandle, false);
            }
        } else {
            free(pContext);
        }
    }

    return errorCodeOrHandle;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise GNSS.
int32_t uDevicePrivateGnssInit()
{
    return uGnssInit();
}

// Deinitialise GNSS.
void uDevicePrivateGnssDeinit()
{
    uGnssDeinit();
}

// Power up a GNSS device, making it available for configuration.
int32_t uDevicePrivateGnssAdd(const uDeviceCfg_t *pDevCfg,
                              uDeviceHandle_t *pDeviceHandle)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    int32_t transportHandle;
    const uDeviceCfgUart_t *pCfgUart;
    const uDeviceCfgI2c_t *pCfgI2c;
    const uDeviceCfgGnss_t *pCfgGnss;

    if ((pDevCfg != NULL) && (pDeviceHandle != NULL)) {
        pCfgGnss = &(pDevCfg->deviceCfg.cfgGnss);
        if (pCfgGnss->version == 0) {
            switch (pDevCfg->transportType) {
                case U_DEVICE_TRANSPORT_TYPE_UART:
                    pCfgUart = &(pDevCfg->transportCfg.cfgUart);
                    // Open a UART with the recommended buffer length
                    // and default baud rate.
                    errorCode = uPortUartOpen(pCfgUart->uart,
                                              pCfgUart->baudRate, NULL,
                                              U_GNSS_UART_BUFFER_LENGTH_BYTES,
                                              pCfgUart->pinTxd,
                                              pCfgUart->pinRxd,
                                              pCfgUart->pinCts,
                                              pCfgUart->pinRts);
                    if (errorCode >= 0) {
                        transportHandle = errorCode;
                        errorCode = addDevice(transportHandle,
                                              pDevCfg->transportType,
                                              pCfgGnss, pDeviceHandle);
                        if (errorCode < 0) {
                            // Clean up on error
                            uPortUartClose(transportHandle);
                        }
                    }
                    break;
                case U_DEVICE_TRANSPORT_TYPE_I2C:
                    pCfgI2c = &(pDevCfg->transportCfg.cfgI2c);
                    // Open an I2C instance.
                    errorCode = uPortI2cOpen(pCfgI2c->i2c, pCfgI2c->pinSda,
                                             pCfgI2c->pinScl, true);
                    if (errorCode >= 0) {
                        transportHandle = errorCode;
                        errorCode = addDevice(transportHandle,
                                              pDevCfg->transportType,
                                              pCfgGnss, pDeviceHandle);
                        if (errorCode < 0) {
                            // Clean up on error
                            uPortI2cClose(transportHandle);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    return errorCode;
}

// Remove a GNSS device.
int32_t uDevicePrivateGnssRemove(uDeviceHandle_t devHandle,
                                 bool powerOff)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
    uDeviceGnssInstance_t *pContext = (uDeviceGnssInstance_t *) U_DEVICE_INSTANCE(devHandle)->pContext;
    int32_t transportHandle;
    uDeviceTransportType_t transportType;

    if (pContext != NULL) {
        transportHandle = pContext->transportHandle;
        transportType = pContext->transportType;
        errorCode = removeDevice(devHandle, powerOff);
        if (errorCode == 0) {
            // Having removed the device, close the transport
            switch (transportType) {
                case U_DEVICE_TRANSPORT_TYPE_UART:
                    uPortUartClose(transportHandle);
                    break;
                case U_DEVICE_TRANSPORT_TYPE_I2C:
                    uPortI2cClose(transportHandle);
                    break;
                default:
                    break;
            }
        }
    }

    return errorCode;
}

// End of file
