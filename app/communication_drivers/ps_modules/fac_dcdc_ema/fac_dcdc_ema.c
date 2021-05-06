/******************************************************************************
 * Copyright (C) 2018 by LNLS - Brazilian Synchrotron Light Laboratory
 *
 * Redistribution, modification or use of this software in source or binary
 * forms is permitted as long as the files maintain this copyright. LNLS and
 * the Brazilian Center for Research in Energy and Materials (CNPEM) are not
 * liable for any misuse of this material.
 *
 *****************************************************************************/

/**
 * @file fac_dcdc_ema.c
 * @brief FAC DC/DC Stage module for dipole magnet from EMA beamline.
 *
 * Module for control of DC/DC module of FAC power supply for dipole magnet used
 * at EMA beamline from Sirius. It implements the controller for load current.
 *
 * @author gabriel.brunheira
 * @date 08/03/2019
 *
 */

#include <stdint.h>
#include <stdbool.h>

#include "inc/hw_memmap.h"
#include "inc/hw_ipc.h"
#include "inc/hw_types.h"

#include "communication_drivers/ipc/ipc_lib.h"
#include "communication_drivers/adcp/adcp.h"
#include "communication_drivers/bsmp/bsmp_lib.h"
#include "communication_drivers/can/can_bkp.h"
#include "communication_drivers/control/control.h"
#include "communication_drivers/control/wfmref/wfmref.h"
#include "communication_drivers/event_manager/event_manager.h"
#include "communication_drivers/iib/iib_data.h"
#include "communication_drivers/iib/iib_module.h"
#include "communication_drivers/ps_modules/fac_dcdc_ema/fac_dcdc_ema.h"
#include "communication_drivers/ps_modules/ps_modules.h"

#define I_LOAD                      g_controller_ctom.net_signals[0]  // HRADC0
#define V_DCLINK                    g_controller_ctom.net_signals[1]  // HRADC1

#define I_LOAD_ERROR                g_controller_ctom.net_signals[2]

#define DUTY_I_LOAD_PI              g_controller_ctom.net_signals[3]
#define DUTY_REFERENCE_FF           g_controller_ctom.net_signals[4]
#define DUTY_NOMINAL                g_controller_ctom.net_signals[5]

#define V_CAPBANK_FILTERED          g_controller_ctom.net_signals[6]

#define DUTY_CYCLE                  g_controller_ctom.output_signals[0]

/**
 * Interlocks defines
 */
typedef enum
{
    Load_Overcurrent,
    DCLink_Overvoltage,
    DCLink_Undervoltage,
    Emergency_Button,
    Load_Waterflow,
    Load_Overtemperature,
    IIB_Itlk,
} hard_interlocks_t;

typedef enum
{
    DCCT_Fault,
    Load_Feedback_Fault,
} soft_interlocks_t;

volatile iib_fac_os_t iib_fac_os;

static void init_iib_modules();

static void handle_can_data(uint8_t *data);
static void handle_can_interlock(uint8_t *data);
static void handle_can_alarm(uint8_t *data);

/**
* @brief Initialize ADCP Channels.
*
* Setup ADCP specific parameters for FAC ACDC operation.
*
*/
static void adcp_channel_config(void)
{
    g_analog_ch_0.Enable = 0;
    g_analog_ch_1.Enable = 0;
    g_analog_ch_2.Enable = 0;
    g_analog_ch_3.Enable = 0;
    g_analog_ch_4.Enable = 0;
    g_analog_ch_5.Enable = 0;
    g_analog_ch_6.Enable = 0;
    g_analog_ch_7.Enable = 0;
}

/**
* @brief Initialize BSMP servers.
*
* Setup BSMP servers for FBP operation.
*
*/
static void bsmp_init_server(void)
{
    create_bsmp_var(31, 0, 4, false, g_ipc_ctom.ps_module[0].ps_soft_interlock.u8);
    create_bsmp_var(32, 0, 4, false, g_ipc_ctom.ps_module[0].ps_hard_interlock.u8);

    create_bsmp_var(33, 0, 4, false, I_LOAD.u8);
    create_bsmp_var(34, 0, 4, false, V_DCLINK.u8);

    create_bsmp_var(35, 0, 4, false, DUTY_CYCLE.u8);

    create_bsmp_var(36, 0, 4, false, iib_fac_os.VdcLink.u8);
    create_bsmp_var(37, 0, 4, false, iib_fac_os.Iin.u8);
    create_bsmp_var(38, 0, 4, false, iib_fac_os.Iout.u8);
    create_bsmp_var(39, 0, 4, false, iib_fac_os.TempIGBT1.u8);
    create_bsmp_var(40, 0, 4, false, iib_fac_os.TempIGBT2.u8);
    create_bsmp_var(41, 0, 4, false, iib_fac_os.TempL.u8);
    create_bsmp_var(42, 0, 4, false, iib_fac_os.TempHeatSink.u8);
    create_bsmp_var(43, 0, 4, false, iib_fac_os.DriverVoltage.u8);
    create_bsmp_var(44, 0, 4, false, iib_fac_os.Driver1Current.u8);
    create_bsmp_var(45, 0, 4, false, iib_fac_os.Driver2Current.u8);
    create_bsmp_var(46, 0, 4, false, iib_fac_os.GroundLeakage.u8);
    create_bsmp_var(47, 0, 4, false, iib_fac_os.BoardTemperature.u8);
    create_bsmp_var(48, 0, 4, false, iib_fac_os.RelativeHumidity.u8);
    create_bsmp_var(49, 0, 4, false, iib_fac_os.InterlocksRegister.u8);
    create_bsmp_var(50, 0, 4, false, iib_fac_os.AlarmsRegister.u8);
}

/**
* @brief System configuration for FBP.
*
* Initialize specific parameters e configure peripherals for FBP operation.
*
*/
void fac_dcdc_ema_system_config()
{
    adcp_channel_config();
    bsmp_init_server();
    init_iib_modules();

    init_wfmref(&WFMREF[0], WFMREF_SELECTED_PARAM[0].u16,
                WFMREF_SYNC_MODE_PARAM[0].u16, ISR_CONTROL_FREQ.f,
                WFMREF_FREQUENCY_PARAM[0].f, WFMREF_GAIN_PARAM[0].f,
                WFMREF_OFFSET_PARAM[0].f, &g_wfmref_data.data[0][0].f,
                SIZE_WFMREF, &g_ipc_ctom.ps_module[0].ps_reference.f);

    init_scope(&g_ipc_mtoc.scope[0], ISR_CONTROL_FREQ.f,
               SCOPE_FREQ_SAMPLING_PARAM[0].f, &(g_buf_samples_ctom[0].f),
               SIZE_BUF_SAMPLES_CTOM, SCOPE_SOURCE_PARAM[0].p_f,
               (void *) 0);
}

static void init_iib_modules()
{
    iib_fac_os.CanAddress = 1;

    init_iib_module_can_data(&g_iib_module_can_data, &handle_can_data);
    init_iib_module_can_interlock(&g_iib_module_can_interlock, &handle_can_interlock);
    init_iib_module_can_alarm(&g_iib_module_can_alarm, &handle_can_alarm);
}

static void handle_can_data(uint8_t *data)
{
    switch(data[1])
    {
        case 0:
        {
            memcpy(iib_fac_os.VdcLink.u8, &data[4], 4);
            break;
        }
        case 1:
        {
            memcpy(iib_fac_os.Iin.u8, &data[4], 4);
            break;
        }
        case 2:
        {
            memcpy(iib_fac_os.Iout.u8, &data[4], 4);
            break;
        }
        case 3:
        {
            memcpy(iib_fac_os.TempIGBT1.u8, &data[4], 4);
            break;
        }
        case 4:
        {
            memcpy(iib_fac_os.TempIGBT2.u8, &data[4], 4);
            break;
        }
        case 5:
        {
            memcpy(iib_fac_os.DriverVoltage.u8, &data[4], 4);
            break;
        }
        case 6:
        {
            memcpy(iib_fac_os.Driver1Current.u8, &data[4], 4);
            break;
        }
        case 7:
        {
            memcpy(iib_fac_os.Driver2Current.u8, &data[4], 4);
            break;
        }
        case 8:
        {
            memcpy(iib_fac_os.GroundLeakage.u8, &data[4], 4);
            break;
        }
        case 9:
        {
            memcpy(iib_fac_os.TempL.u8, &data[4], 4);
            break;
        }
        case 10:
        {
            memcpy(iib_fac_os.TempHeatSink.u8, &data[4], 4);
            break;
        }
        case 11:
        {
            memcpy(iib_fac_os.BoardTemperature.u8, &data[4], 4);
            break;
        }
        case 12:
        {
            memcpy(iib_fac_os.RelativeHumidity.u8, &data[4], 4);
            break;
        }
        default:
        {
            break;
        }
    }
}

static void handle_can_interlock(uint8_t *data)
{
    switch(data[1])
    {
       case 0:
       {
           if(g_can_reset_flag[0])
           {
               memcpy(iib_fac_os.InterlocksRegister.u8, &data[4], 4);
               set_hard_interlock(0, IIB_Itlk);
           }
           break;
       }

       case 1:
       {
           g_can_reset_flag[0] = 1;
           iib_fac_os.InterlocksRegister.u32 = 0;
           break;
       }

       default:
       {
           break;
       }
    }
}

static void handle_can_alarm(uint8_t *data)
{
    switch(data[1])
    {
       case 0:
       {
           memcpy(iib_fac_os.AlarmsRegister.u8, &data[4], 4);
           break;
       }

       case 1:
       {
           iib_fac_os.AlarmsRegister.u32 = 0;
           break;
       }

       default:
       {
           break;
       }
    }
}
