/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie Firmware
 *
 * Copyright (C) 2011-2016 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */
#define DEBUG_MODULE "STAB"
#include <math.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "debug.h"
#include "config.h"
#include "system.h"
#include "pm.h"
#include "stabilizer.h"
#include "commander.h"
#include "attitude_controller.h"
#include "sensfusion6.h"
#include "imu.h"
#include "motors.h"
#include "log.h"
#include "pid.h"
#include "param.h"
#include "sitaw.h"
#ifdef PLATFORM_CF1
  #include "ms5611.h"
#else
  #include "lps25h.h"
#endif
#include "num.h"
#include "position_estimator.h"
#include "position_controller.h"
#include "altitude_hold.h"
#include "led.h"


/**
 * Defines in what divided update rate should the attitude
 * control loop run relative the rate control loop.
 */
#define ATTITUDE_UPDATE_RATE_DIVIDER  2
#define ATTITUDE_UPDATE_DT  (float)(1.0 / (IMU_UPDATE_FREQ / ATTITUDE_UPDATE_RATE_DIVIDER)) // 250hz

#define ALTHOLD_UPDATE_RATE_DIVIDER  5
#define ALTHOLD_UPDATE_DT  (float)(1.0 / (IMU_UPDATE_FREQ / ALTHOLD_UPDATE_RATE_DIVIDER))   // 100hz

static Axis3f gyro; // Gyro axis data in deg/s
static Axis3f acc;  // Accelerometer axis data in mG
static Axis3f mag;  // Magnetometer axis data in testla

static float eulerRollActual;   // Measured roll angle in deg
static float eulerPitchActual;  // Measured pitch angle in deg
static float eulerYawActual;    // Measured yaw angle in deg
static float eulerRollDesired;  // Desired roll angle in deg
static float eulerPitchDesired; // Desired ptich angle in deg
static float eulerYawDesired;   // Desired yaw angle in deg
uint16_t actuatorThrust;

uint32_t motorPowerM1;  // Motor 1 power output (16bit value used: 0 - 65535)
uint32_t motorPowerM2;  // Motor 2 power output (16bit value used: 0 - 65535)
uint32_t motorPowerM3;  // Motor 3 power output (16bit value used: 0 - 65535)
uint32_t motorPowerM4;  // Motor 4 power output (16bit value used: 0 - 65535)

static float reference[4]; // thetaR,thetaP,z',thetaY'
static float referenceOut[4]; // reference multiplied with Kr
static float sensors[8]; // z,thetaR,thetaP,thetaY,z',thetaR',thetaP',thetaY'
static float sensorsOut[4];	// sensors multiplied with K
static int32_t controlMotor[4]; // controllsignal for motors expressed in pwm
//static int32_t whichMode = 0;	// For debugging, to set desired mode also change in stabilizerTask
								// and in PARAM at the bottom
static int32_t refAngle = 20;	// Input signal for reference angle

static float thrustOffset = 0.06;  // Thrust per motor in order to reach equilibrium
static float K[4][8];
static float Kr[4][4];

// Sport-mode
const float K_sport[4][8]={
		{-0.0000050, -0.0007071, 0.0007071, 0.0000050, -0.0002646, -0.00003763, 0.00003763, 0.0000050},
		{-0.0000050, -0.0007071, -0.0007071, -0.0000050, -0.0002646, -0.00003763, -0.00003763, -0.0000050},
		{-0.0000050, 0.0007071, -0.0007071, 0.0000050, -0.0002646, 0.00003763, -0.00003763, 0.0000050},
		{-0.0000050, 0.0007071, 0.0007071, -0.0000050, -0.0002646, 0.00003763, 0.00003763, -0.0000050}
		};
// Sport-mode
const float Kr_sport[4][4]={
		{-0.0007071, 0.0007071, -0.0002646, 0.0000050},
		{-0.0007071, -0.0007071, -0.0002646, -0.0000050},
		{0.0007071, -0.0007071, -0.0002646, 0.0000050},
		{0.0007071, 0.0007071, -0.0002646, -0.0000050}
		};

// Eco-mode
const float K_eco[4][8]={
		{-0.0000050, -0.0005000, 0.0005000, 0.0000050, -0.0002646, -0.00003164, 0.00003164, 0.0000050},
		{-0.0000050, -0.0005000, -0.0005000, -0.0000050, -0.0002646, -0.00003164, -0.00003164, -0.0000050},
		{-0.0000050, 0.0005000, -0.0005000, 0.0000050, -0.0002646, 0.00003164, -0.00003164, 0.0000050},
		{-0.0000050, 0.0005000, 0.0005000, -0.0000050, -0.0002646, 0.00003164, 0.00003164, -0.0000050}
		};
// Eco-mode
const float Kr_eco[4][4]={
		{-0.0005000, 0.0005000, -0.0002646, 0.0000050},
		{-0.0005000, -0.0005000, -0.0002646, -0.0000050},
		{0.0005000, -0.0005000, -0.0002646, 0.0000050},
		{0.0005000, 0.0005000, -0.0002646, -0.0000050}
		};

static bool isInit;
static bool isInitModeSwitcher;
static bool isInitRefMaker;

QueueHandle_t xQueueMode;		// Queue for mode switch
xSemaphoreHandle refSemaphore = 0;	// Semaphore for reference

static uint16_t limitThrust(int32_t value);
void updateLQController(void);

static void stabilizerTask(void* param)
{
  //uint32_t motorCounter;
  uint32_t modeCounter;
  uint32_t lastWakeTime;
  // Copy K_eco and Kr_eco to K and Kr in order to start using
  // these controller parameters
  memcpy(&K, &K_eco, sizeof K);
  memcpy(&Kr, &Kr_eco, sizeof Kr);

  vTaskSetApplicationTaskTag(0, (void*)TASK_STABILIZER_ID_NBR);

  //Wait for the system to be fully started to start stabilization loop
  systemWaitStart();

  lastWakeTime = xTaskGetTickCount ();

  while(1)
  {
    vTaskDelayUntil(&lastWakeTime, F2T(IMU_UPDATE_FREQ)); // 500Hz

    // Magnetometer not yet used more then for logging.
    imu9Read(&gyro, &acc, &mag);

    if (imu6IsCalibrated())
    {

    	sensfusion6UpdateQ(gyro.x, gyro.y, gyro.z, acc.x, acc.y, acc.z, ATTITUDE_UPDATE_DT);
    	sensfusion6GetEulerRPY(&eulerRollActual, &eulerPitchActual, &eulerYawActual);
    	// Since not all states are interresting at the moment these are set to zero
    	// but still here so that it's easy to change
    	sensors[0] = 0;
    	sensors[1] = eulerRollActual;
    	sensors[2] = eulerPitchActual;
    	sensors[3] = 0; //-eulerYawActual;
    	sensors[4] = 0;
    	sensors[5] = gyro.x;
    	sensors[6] = -gyro.y;
    	sensors[7] = 0; //-gyro.z;

    	// If there exists values in queue receive them
    	if (xQueueReceive(xQueueMode, &modeCounter, M2T(10))) {

    		// For debugging reasons. In order to change mode from
			// CF-client
			//modeCounter = whichMode;

    		// Set the desired controller
			if (modeCounter == 0) {
				memcpy(&K, &K_eco, sizeof K);
				memcpy(&Kr, &Kr_eco, sizeof Kr);
			}
			else if (modeCounter == 1) {
				memcpy(&K, &K_sport, sizeof K);
				memcpy(&Kr, &Kr_sport, sizeof Kr);
			}
		}

    	// Update control signal
    	updateLQController();

    	motorsSetRatio(MOTOR_M1, motorPowerM1);
    	motorsSetRatio(MOTOR_M2, motorPowerM2);
    	motorsSetRatio(MOTOR_M3, motorPowerM3);
    	motorsSetRatio(MOTOR_M4, motorPowerM4);

    }
  }
}

// Function for updating control signal
void updateLQController(void) {

	commanderGetThrust(&actuatorThrust);
	commanderGetRPY(&eulerRollDesired, &eulerPitchDesired, &eulerYawDesired);

	reference[0] = eulerRollDesired;
	reference[1] = eulerPitchDesired;
	reference[3] = 0;
	reference[4] = 0;

	referenceOut[0] = Kr[0][0]*reference[0]+Kr[0][1]*reference[1]+Kr[0][2]*reference[2]+Kr[0][3]*reference[3];
	referenceOut[1] = Kr[1][0]*reference[0]+Kr[1][1]*reference[1]+Kr[1][2]*reference[2]+Kr[1][3]*reference[3];
	referenceOut[2] = Kr[2][0]*reference[0]+Kr[2][1]*reference[1]+Kr[2][2]*reference[2]+Kr[2][3]*reference[3];
	referenceOut[3] = Kr[3][0]*reference[0]+Kr[3][1]*reference[1]+Kr[3][2]*reference[2]+Kr[3][3]*reference[3];

	// Calculate matrix multiplication
	sensorsOut[0] = K[0][0]*sensors[0]+K[0][1]*sensors[1]+K[0][2]*sensors[2]+K[0][3]*sensors[3]+K[0][4]*sensors[4]+K[0][5]*sensors[5]+K[0][6]*sensors[6]+K[0][7]*sensors[7];
	sensorsOut[1] = K[1][0]*sensors[0]+K[1][1]*sensors[1]+K[1][2]*sensors[2]+K[1][3]*sensors[3]+K[1][4]*sensors[4]+K[1][5]*sensors[5]+K[1][6]*sensors[6]+K[1][7]*sensors[7];
	sensorsOut[2] = K[2][0]*sensors[0]+K[2][1]*sensors[1]+K[2][2]*sensors[2]+K[2][3]*sensors[3]+K[2][4]*sensors[4]+K[2][5]*sensors[5]+K[2][6]*sensors[6]+K[2][7]*sensors[7];
	sensorsOut[3] = K[3][0]*sensors[0]+K[3][1]*sensors[1]+K[3][2]*sensors[2]+K[3][3]*sensors[3]+K[3][4]*sensors[4]+K[3][5]*sensors[5]+K[3][6]*sensors[6]+K[3][7]*sensors[7];

	// Mapping thrust in Newtons to pwm
	controlMotor[0] = (int32_t)((actuatorThrust + referenceOut[0]-sensorsOut[0]) * 1000.0 * 1092.0 * 4 / 9.81);
	controlMotor[1] = (int32_t)((actuatorThrust + referenceOut[1]-sensorsOut[1]) * 1000.0 * 1092.0 * 4 / 9.81);
	controlMotor[2] = (int32_t)((actuatorThrust + referenceOut[2]-sensorsOut[2]) * 1000.0 * 1092.0 * 4 / 9.81);
	controlMotor[3] = (int32_t)((actuatorThrust + referenceOut[3]-sensorsOut[3]) * 1000.0 * 1092.0 * 4 / 9.81);

	// Set desired motor power
	motorPowerM1 = limitThrust(controlMotor[0]);
	motorPowerM2 = limitThrust(controlMotor[1]);
	motorPowerM3 = limitThrust(controlMotor[2]);
	motorPowerM4 = limitThrust(controlMotor[3]);
}

static uint16_t limitThrust(int32_t value)
{
  return limitUint16(value);
}

void stabilizerInit(void)
{
  if(isInit)
    return;

  motorsInit(motorMapDefaultBrushed);
  imu6Init();
  sensfusion6Init();
  attitudeControllerInit();
  modeSwitcherInit();
  refMakerInit();

  xTaskCreate(stabilizerTask, STABILIZER_TASK_NAME,
              STABILIZER_TASK_STACKSIZE, NULL, STABILIZER_TASK_PRI, NULL);

  isInit = true;
}

bool stabilizerTest(void)
{
  bool pass = true;

  pass &= motorsTest();
  pass &= imu6Test();
  pass &= sensfusion6Test();
  pass &= attitudeControllerTest();

  return pass;
}

void modeSwitcher(void* param)
{
	uint32_t lastWakeTime;
	uint32_t modeCounter = 0;
	systemWaitStart();
	lastWakeTime = xTaskGetTickCount ();

	while(1)
	  {
		vTaskDelayUntil(&lastWakeTime, M2T(16000)); // Wait 16 seconds
		if (modeCounter == 0) {
			modeCounter = 1;
		}
		else {
			modeCounter = 0;
		}
	    xQueueSendToBack(xQueueMode, &modeCounter, M2T(10));
	  }

}

void modeSwitcherInit(void)
{
	if(isInitModeSwitcher)
	    return;

	xTaskCreate(modeSwitcher, MODE_SWITCHER_TASK_NAME,
			MODE_SWITCHER_STACKSIZE, NULL, MODE_SWITCHER_TASK_PRI, NULL);
	xQueueMode = xQueueCreate(1,sizeof(uint32_t));

	isInitModeSwitcher = true;
}


void refMaker(void* param)
{
	uint32_t lastWakeTime;
	uint32_t refCounter = 0;
	systemWaitStart();
	lastWakeTime = xTaskGetTickCount ();

	while(1)
	  {

		if (xSemaphoreTake(refSemaphore,M2T(10))) {
			*(reference+0) = 0;		// thetaR
			*(reference+1) = refCounter*refAngle;		// thetaP
			*(reference+2) = 0;		// z'
			*(reference+3) = 0;		// thetaY'
			xSemaphoreGive(refSemaphore);
		}

		if (refCounter == 0)
			refCounter = 1;
		else
			refCounter = 0;

		vTaskDelayUntil(&lastWakeTime, M2T(8000)); // Wait 8 seconds

	  }
}

void refMakerInit(void)
{
	if(isInitRefMaker)
	    return;

	xTaskCreate(refMaker, REF_MAKER_TASK_NAME,
			REF_MAKER_STACKSIZE, NULL, REF_MAKER_TASK_PRI, NULL);
	refSemaphore = xSemaphoreCreateMutex();

	isInitRefMaker = true;
}


LOG_GROUP_START(stabilizer)
LOG_ADD(LOG_FLOAT, roll, &eulerRollActual)
LOG_ADD(LOG_FLOAT, pitch, &eulerPitchActual)
LOG_ADD(LOG_FLOAT, yaw, &eulerYawActual)
LOG_GROUP_STOP(stabilizer)

LOG_GROUP_START(acc)
LOG_ADD(LOG_FLOAT, x, &acc.x)
LOG_ADD(LOG_FLOAT, y, &acc.y)
LOG_ADD(LOG_FLOAT, z, &acc.z)
LOG_GROUP_STOP(acc)

LOG_GROUP_START(gyro)
LOG_ADD(LOG_FLOAT, x, &gyro.x)
LOG_ADD(LOG_FLOAT, y, &gyro.y)
LOG_ADD(LOG_FLOAT, z, &gyro.z)
LOG_GROUP_STOP(gyro)

LOG_GROUP_START(mag)
LOG_ADD(LOG_FLOAT, x, &mag.x)
LOG_ADD(LOG_FLOAT, y, &mag.y)
LOG_ADD(LOG_FLOAT, z, &mag.z)
LOG_GROUP_STOP(mag)

LOG_GROUP_START(motor)
LOG_ADD(LOG_INT32, m4, &motorPowerM4)
LOG_ADD(LOG_INT32, m1, &motorPowerM1)
LOG_ADD(LOG_INT32, m2, &motorPowerM2)
LOG_ADD(LOG_INT32, m3, &motorPowerM3)
LOG_GROUP_STOP(motor)

LOG_GROUP_START(controller)
LOG_ADD(LOG_FLOAT, sens1, &sensorsOut[0])
LOG_ADD(LOG_FLOAT, sens2, &sensorsOut[1])
LOG_ADD(LOG_FLOAT, sens3, &sensorsOut[2])
LOG_ADD(LOG_FLOAT, sens4, &sensorsOut[3])
LOG_GROUP_STOP(controller)

LOG_GROUP_START(reference)
LOG_ADD(LOG_FLOAT, sens1, &reference[0])
LOG_ADD(LOG_FLOAT, sens2, &reference[1])
LOG_ADD(LOG_FLOAT, sens3, &reference[2])
LOG_ADD(LOG_FLOAT, sens4, &reference[3])
LOG_GROUP_STOP(reference)

PARAM_GROUP_START(thrust)
PARAM_ADD(PARAM_FLOAT, thrust, &thrustOffset)
PARAM_ADD(PARAM_INT32, refAngle, &refAngle)
//PARAM_ADD(PARAM_INT32, whichMode, &whichMode)
PARAM_GROUP_STOP(thrust)
