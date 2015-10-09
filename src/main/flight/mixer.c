/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <platform.h>
#include "debug.h"

#include "build_config.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "drivers/system.h"
#include "drivers/pwm_output.h"
#include "drivers/pwm_mapping.h"
#include "drivers/sensor.h"
#include "drivers/accgyro.h"
#include "drivers/system.h"

#include "rx/rx.h"

#include "io/gimbal.h"
#include "io/escservo.h"
#include "io/rc_controls.h"

#include "sensors/sensors.h"
#include "sensors/acceleration.h"

#include "flight/mixer.h"
#include "flight/failsafe.h"
#include "flight/pid.h"
#include "flight/imu.h"

#include "config/runtime_config.h"
#include "config/config.h"

#define INT_PRECISION               (1024)

#ifdef USE_SERVOS
#define TRI_TAIL_SERVO_ANGLE_MID_DD (900)
#define TRI_YAW_FORCE_CURVE_SIZE    (100)
#define TRI_TAIL_SERVO_MAX_ANGLE_DD (500)

// These need to be configurable
#define TRI_TAIL_MOTOR_CURVE_MAX_PHASE_SHIFT_DEGREES    15.0f

#endif

//#define MIXER_DEBUG

uint8_t motorCount;

int16_t motor[MAX_SUPPORTED_MOTORS];
int16_t motor_disarmed[MAX_SUPPORTED_MOTORS];

bool motorLimitReached;

static mixerConfig_t *mixerConfig;
static flight3DConfig_t *flight3DConfig;
static escAndServoConfig_t *escAndServoConfig;
static airplaneConfig_t *airplaneConfig;
static rxConfig_t *rxConfig;

static mixerMode_e currentMixerMode;
static motorMixer_t currentMixer[MAX_SUPPORTED_MOTORS];

#ifdef USE_SERVOS
static uint8_t servoRuleCount = 0;
static servoMixer_t currentServoMixer[MAX_SERVO_RULES];
static gimbalConfig_t *gimbalConfig;
int16_t servo[MAX_SUPPORTED_SERVOS];
static int useServo;
STATIC_UNIT_TESTED uint8_t servoCount;
static servoParam_t *servoConf;
static biquad_t servoFilterState[MAX_SUPPORTED_SERVOS];
#endif

static const motorMixer_t mixerQuadX[] = {
    { 1.0f, -1.0f,  1.0f, -1.0f },          // REAR_R
    { 1.0f, -1.0f, -1.0f,  1.0f },          // FRONT_R
    { 1.0f,  1.0f,  1.0f,  1.0f },          // REAR_L
    { 1.0f,  1.0f, -1.0f, -1.0f },          // FRONT_L
};
#ifndef USE_QUAD_MIXER_ONLY
static const motorMixer_t mixerTricopter[] = {
    { 1.0f,  0.0f,  1.333333f,  0.0f },     // REAR
    { 1.0f, -1.0f, -0.666667f,  0.0f },     // RIGHT
    { 1.0f,  1.0f, -0.666667f,  0.0f },     // LEFT
};

static const motorMixer_t mixerQuadP[] = {
    { 1.0f,  0.0f,  1.0f, -1.0f },          // REAR
    { 1.0f, -1.0f,  0.0f,  1.0f },          // RIGHT
    { 1.0f,  1.0f,  0.0f,  1.0f },          // LEFT
    { 1.0f,  0.0f, -1.0f, -1.0f },          // FRONT
};

static const motorMixer_t mixerBicopter[] = {
    { 1.0f,  1.0f,  0.0f,  0.0f },          // LEFT
    { 1.0f, -1.0f,  0.0f,  0.0f },          // RIGHT
};

static const motorMixer_t mixerY6[] = {
    { 1.0f,  0.0f,  1.333333f,  1.0f },     // REAR
    { 1.0f, -1.0f, -0.666667f, -1.0f },     // RIGHT
    { 1.0f,  1.0f, -0.666667f, -1.0f },     // LEFT
    { 1.0f,  0.0f,  1.333333f, -1.0f },     // UNDER_REAR
    { 1.0f, -1.0f, -0.666667f,  1.0f },     // UNDER_RIGHT
    { 1.0f,  1.0f, -0.666667f,  1.0f },     // UNDER_LEFT
};

static const motorMixer_t mixerHex6P[] = {
    { 1.0f, -0.866025f,  0.5f,  1.0f },     // REAR_R
    { 1.0f, -0.866025f, -0.5f, -1.0f },     // FRONT_R
    { 1.0f,  0.866025f,  0.5f,  1.0f },     // REAR_L
    { 1.0f,  0.866025f, -0.5f, -1.0f },     // FRONT_L
    { 1.0f,  0.0f,      -1.0f,  1.0f },     // FRONT
    { 1.0f,  0.0f,       1.0f, -1.0f },     // REAR
};

static const motorMixer_t mixerY4[] = {
    { 1.0f,  0.0f,  1.0f, -1.0f },          // REAR_TOP CW
    { 1.0f, -1.0f, -1.0f,  0.0f },          // FRONT_R CCW
    { 1.0f,  0.0f,  1.0f,  1.0f },          // REAR_BOTTOM CCW
    { 1.0f,  1.0f, -1.0f,  0.0f },          // FRONT_L CW
};

static const motorMixer_t mixerHex6X[] = {
    { 1.0f, -0.5f,  0.866025f,  1.0f },     // REAR_R
    { 1.0f, -0.5f, -0.866025f,  1.0f },     // FRONT_R
    { 1.0f,  0.5f,  0.866025f, -1.0f },     // REAR_L
    { 1.0f,  0.5f, -0.866025f, -1.0f },     // FRONT_L
    { 1.0f, -1.0f,  0.0f,      -1.0f },     // RIGHT
    { 1.0f,  1.0f,  0.0f,       1.0f },     // LEFT
};

static const motorMixer_t mixerOctoX8[] = {
    { 1.0f, -1.0f,  1.0f, -1.0f },          // REAR_R
    { 1.0f, -1.0f, -1.0f,  1.0f },          // FRONT_R
    { 1.0f,  1.0f,  1.0f,  1.0f },          // REAR_L
    { 1.0f,  1.0f, -1.0f, -1.0f },          // FRONT_L
    { 1.0f, -1.0f,  1.0f,  1.0f },          // UNDER_REAR_R
    { 1.0f, -1.0f, -1.0f, -1.0f },          // UNDER_FRONT_R
    { 1.0f,  1.0f,  1.0f, -1.0f },          // UNDER_REAR_L
    { 1.0f,  1.0f, -1.0f,  1.0f },          // UNDER_FRONT_L
};

static const motorMixer_t mixerOctoFlatP[] = {
    { 1.0f,  0.707107f, -0.707107f,  1.0f },    // FRONT_L
    { 1.0f, -0.707107f, -0.707107f,  1.0f },    // FRONT_R
    { 1.0f, -0.707107f,  0.707107f,  1.0f },    // REAR_R
    { 1.0f,  0.707107f,  0.707107f,  1.0f },    // REAR_L
    { 1.0f,  0.0f, -1.0f, -1.0f },              // FRONT
    { 1.0f, -1.0f,  0.0f, -1.0f },              // RIGHT
    { 1.0f,  0.0f,  1.0f, -1.0f },              // REAR
    { 1.0f,  1.0f,  0.0f, -1.0f },              // LEFT
};

static const motorMixer_t mixerOctoFlatX[] = {
    { 1.0f,  1.0f, -0.414178f,  1.0f },      // MIDFRONT_L
    { 1.0f, -0.414178f, -1.0f,  1.0f },      // FRONT_R
    { 1.0f, -1.0f,  0.414178f,  1.0f },      // MIDREAR_R
    { 1.0f,  0.414178f,  1.0f,  1.0f },      // REAR_L
    { 1.0f,  0.414178f, -1.0f, -1.0f },      // FRONT_L
    { 1.0f, -1.0f, -0.414178f, -1.0f },      // MIDFRONT_R
    { 1.0f, -0.414178f,  1.0f, -1.0f },      // REAR_R
    { 1.0f,  1.0f,  0.414178f, -1.0f },      // MIDREAR_L
};

static const motorMixer_t mixerVtail4[] = {
    { 1.0f,  -0.58f,  0.58f, 1.0f },        // REAR_R
    { 1.0f,  -0.46f, -0.39f, -0.5f },       // FRONT_R
    { 1.0f,  0.58f,  0.58f, -1.0f },        // REAR_L
    { 1.0f,  0.46f, -0.39f, 0.5f },         // FRONT_L
};

static const motorMixer_t mixerAtail4[] = {
    { 1.0f,  0.0f,  1.0f,  1.0f },          // REAR_R
    { 1.0f, -1.0f, -1.0f,  0.0f },          // FRONT_R
    { 1.0f,  0.0f,  1.0f, -1.0f },          // REAR_L
    { 1.0f,  1.0f, -1.0f, -0.0f },          // FRONT_L
};

static const motorMixer_t mixerHex6H[] = {
    { 1.0f, -1.0f,  1.0f, -1.0f },     // REAR_R
    { 1.0f, -1.0f, -1.0f,  1.0f },     // FRONT_R
    { 1.0f,  1.0f,  1.0f,  1.0f },     // REAR_L
    { 1.0f,  1.0f, -1.0f, -1.0f },     // FRONT_L
    { 1.0f,  0.0f,  0.0f,  0.0f },     // RIGHT
    { 1.0f,  0.0f,  0.0f,  0.0f },     // LEFT
};

static const motorMixer_t mixerDualcopter[] = {
    { 1.0f,  0.0f,  0.0f, -1.0f },          // LEFT
    { 1.0f,  0.0f,  0.0f,  1.0f },          // RIGHT
};

static const motorMixer_t mixerSingleProp[] = {
    { 1.0f,  0.0f,  0.0f, 0.0f },
};

// Keep synced with mixerMode_e
const mixer_t mixers[] = {
    // motors, use servo, motor mixer
    { 0, false, NULL },                // entry 0
    { 3, true,  mixerTricopter },      // MIXER_TRI
    { 4, false, mixerQuadP },          // MIXER_QUADP
    { 4, false, mixerQuadX },          // MIXER_QUADX
    { 2, true,  mixerBicopter },       // MIXER_BICOPTER
    { 0, true,  NULL },                // * MIXER_GIMBAL
    { 6, false, mixerY6 },             // MIXER_Y6
    { 6, false, mixerHex6P },          // MIXER_HEX6
    { 1, true,  mixerSingleProp },     // * MIXER_FLYING_WING
    { 4, false, mixerY4 },             // MIXER_Y4
    { 6, false, mixerHex6X },          // MIXER_HEX6X
    { 8, false, mixerOctoX8 },         // MIXER_OCTOX8
    { 8, false, mixerOctoFlatP },      // MIXER_OCTOFLATP
    { 8, false, mixerOctoFlatX },      // MIXER_OCTOFLATX
    { 1, true,  mixerSingleProp },     // * MIXER_AIRPLANE
    { 0, true,  NULL },                // * MIXER_HELI_120_CCPM
    { 0, true,  NULL },                // * MIXER_HELI_90_DEG
    { 4, false, mixerVtail4 },         // MIXER_VTAIL4
    { 6, false, mixerHex6H },          // MIXER_HEX6H
    { 0, true,  NULL },                // * MIXER_PPM_TO_SERVO
    { 2, true,  mixerDualcopter },     // MIXER_DUALCOPTER
    { 1, true,  NULL },                // MIXER_SINGLECOPTER
    { 4, false, mixerAtail4 },         // MIXER_ATAIL4
    { 0, false, NULL },                // MIXER_CUSTOM
    { 2, true,  NULL },                // MIXER_CUSTOM_AIRPLANE
    { 3, true,  NULL },                // MIXER_CUSTOM_TRI
};
#endif

#ifdef USE_SERVOS

#define COUNT_SERVO_RULES(rules) (sizeof(rules) / sizeof(servoMixer_t))
// mixer rule format servo, input, rate, speed, min, max, box
static const servoMixer_t servoMixerAirplane[] = {
    { SERVO_FLAPPERON_1, INPUT_STABILIZED_ROLL,  100, 0, 0, 100, 0 },
    { SERVO_FLAPPERON_2, INPUT_STABILIZED_ROLL,  100, 0, 0, 100, 0 },
    { SERVO_RUDDER, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
    { SERVO_ELEVATOR, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
    { SERVO_THROTTLE, INPUT_STABILIZED_THROTTLE, 100, 0, 0, 100, 0 },
};

static const servoMixer_t servoMixerFlyingWing[] = {
    { SERVO_FLAPPERON_1, INPUT_STABILIZED_ROLL,  100, 0, 0, 100, 0 },
    { SERVO_FLAPPERON_1, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
    { SERVO_FLAPPERON_2, INPUT_STABILIZED_ROLL,  -100, 0, 0, 100, 0 },
    { SERVO_FLAPPERON_2, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
    { SERVO_THROTTLE, INPUT_STABILIZED_THROTTLE, 100, 0, 0, 100, 0 },
};

static const servoMixer_t servoMixerBI[] = {
    { SERVO_BICOPTER_LEFT, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
    { SERVO_BICOPTER_LEFT, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
    { SERVO_BICOPTER_RIGHT, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
    { SERVO_BICOPTER_RIGHT, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
};

static const servoMixer_t servoMixerTri[] = {
    { SERVO_RUDDER, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
};

static const servoMixer_t servoMixerDual[] = {
    { SERVO_DUALCOPTER_LEFT, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
    { SERVO_DUALCOPTER_RIGHT, INPUT_STABILIZED_ROLL,  100, 0, 0, 100, 0 },
};

static const servoMixer_t servoMixerSingle[] = {
    { SERVO_SINGLECOPTER_1, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
    { SERVO_SINGLECOPTER_1, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
    { SERVO_SINGLECOPTER_2, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
    { SERVO_SINGLECOPTER_2, INPUT_STABILIZED_PITCH, 100, 0, 0, 100, 0 },
    { SERVO_SINGLECOPTER_3, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
    { SERVO_SINGLECOPTER_3, INPUT_STABILIZED_ROLL,  100, 0, 0, 100, 0 },
    { SERVO_SINGLECOPTER_4, INPUT_STABILIZED_YAW,   100, 0, 0, 100, 0 },
    { SERVO_SINGLECOPTER_4, INPUT_STABILIZED_ROLL,  100, 0, 0, 100, 0 },
};

static const servoMixer_t servoMixerGimbal[] = {
    { SERVO_GIMBAL_PITCH, INPUT_GIMBAL_PITCH, 125, 0, 0, 100, 0 },
    { SERVO_GIMBAL_ROLL, INPUT_GIMBAL_ROLL,  125, 0, 0, 100, 0 },
};


const mixerRules_t servoMixers[] = {
    { 0, NULL },                // entry 0
    { COUNT_SERVO_RULES(servoMixerTri), servoMixerTri },       // MULTITYPE_TRI
    { 0, NULL },                // MULTITYPE_QUADP
    { 0, NULL },                // MULTITYPE_QUADX
    { COUNT_SERVO_RULES(servoMixerBI), servoMixerBI },        // MULTITYPE_BI
    { COUNT_SERVO_RULES(servoMixerGimbal), servoMixerGimbal },    // * MULTITYPE_GIMBAL
    { 0, NULL },                // MULTITYPE_Y6
    { 0, NULL },                // MULTITYPE_HEX6
    { COUNT_SERVO_RULES(servoMixerFlyingWing), servoMixerFlyingWing },// * MULTITYPE_FLYING_WING
    { 0, NULL },                // MULTITYPE_Y4
    { 0, NULL },                // MULTITYPE_HEX6X
    { 0, NULL },                // MULTITYPE_OCTOX8
    { 0, NULL },                // MULTITYPE_OCTOFLATP
    { 0, NULL },                // MULTITYPE_OCTOFLATX
    { COUNT_SERVO_RULES(servoMixerAirplane), servoMixerAirplane },  // * MULTITYPE_AIRPLANE
    { 0, NULL },                // * MULTITYPE_HELI_120_CCPM
    { 0, NULL },                // * MULTITYPE_HELI_90_DEG
    { 0, NULL },                // MULTITYPE_VTAIL4
    { 0, NULL },                // MULTITYPE_HEX6H
    { 0, NULL },                // * MULTITYPE_PPM_TO_SERVO
    { COUNT_SERVO_RULES(servoMixerDual), servoMixerDual },      // MULTITYPE_DUALCOPTER
    { COUNT_SERVO_RULES(servoMixerSingle), servoMixerSingle },    // MULTITYPE_SINGLECOPTER
    { 0, NULL },                // MULTITYPE_ATAIL4
    { 0, NULL },                // MULTITYPE_CUSTOM
    { 0, NULL },                // MULTITYPE_CUSTOM_PLANE
    { 0, NULL },                // MULTITYPE_CUSTOM_TRI
};

static servoMixer_t *customServoMixers;
#endif


static motorMixer_t *customMixers;

#ifdef USE_SERVOS
extern float dT;

static int16_t tailServoMaxYawForce = 0;
static float tailServoThrustFactor = 0;
static int16_t tailServoMaxAngle_DD = 0;
static float virtualServoAngle_D = TRI_TAIL_SERVO_ANGLE_MID_DD;
static int16_t yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE];

static void initTailServoSymmetry();
static uint16_t getServoValueAtAngle(servoParam_t *servoConf, uint16_t angle_DD);
static uint16_t getLinearServoValue(servoParam_t *servoConf, uint16_t servoValue);
static float getPitchCorrectionAtTailAngle(float angle);
static uint16_t getAngleFromYawCurveAtForce(int16_t force);
static uint16_t getServoAngleInDeciDegrees(servoParam_t *servoConf, uint16_t servoValue);
static void virtualServoStep(float dT, servoParam_t *servoConf, uint16_t servoValue);
float getVirtualServoAngleInDegrees();
#endif

void mixerUseConfigs(
#ifdef USE_SERVOS
        servoParam_t *servoConfToUse,
        gimbalConfig_t *gimbalConfigToUse,
#endif
        flight3DConfig_t *flight3DConfigToUse,
        escAndServoConfig_t *escAndServoConfigToUse,
        mixerConfig_t *mixerConfigToUse,
        airplaneConfig_t *airplaneConfigToUse,
        rxConfig_t *rxConfigToUse)
{
#ifdef USE_SERVOS
    servoConf = servoConfToUse;
    gimbalConfig = gimbalConfigToUse;
#endif
    flight3DConfig = flight3DConfigToUse;
    escAndServoConfig = escAndServoConfigToUse;
    mixerConfig = mixerConfigToUse;
    airplaneConfig = airplaneConfigToUse;
    rxConfig = rxConfigToUse;

}

#ifdef USE_SERVOS

void mixerInitialiseServoFiltering(uint32_t targetLooptime)
{
    if (mixerConfig->servo_lowpass_enable) {
        for (int servoIdx = 0; servoIdx < MAX_SUPPORTED_SERVOS; servoIdx++) {
            BiQuadNewLpf(mixerConfig->servo_lowpass_freq, &servoFilterState[servoIdx], targetLooptime);
        }
    }
}

int16_t determineServoMiddleOrForwardFromChannel(servoIndex_e servoIndex)
{
    uint8_t channelToForwardFrom = servoConf[servoIndex].forwardFromChannel;

    if (channelToForwardFrom != CHANNEL_FORWARDING_DISABLED && channelToForwardFrom < rxRuntimeConfig.channelCount) {
        return rcData[channelToForwardFrom];
    }

    return servoConf[servoIndex].middle;
}


int servoDirection(int servoIndex, int inputSource)
{
    // determine the direction (reversed or not) from the direction bitfield of the servo
    if (servoConf[servoIndex].reversedSources & (1 << inputSource))
        return -1;
    else
        return 1;
}
#endif

#ifdef USE_SERVOS
void mixerInit(mixerMode_e mixerMode, motorMixer_t *initialCustomMotorMixers, servoMixer_t *initialCustomServoMixers)
{
    currentMixerMode = mixerMode;

    customMixers = initialCustomMotorMixers;
    customServoMixers = initialCustomServoMixers;

    // enable servos for mixes that require them. note, this shifts motor counts.
    useServo = mixers[currentMixerMode].useServo;
    // if we want camstab/trig, that also enables servos, even if mixer doesn't
    if (feature(FEATURE_SERVO_TILT))
        useServo = 1;

    // give all servos a default command
    for (uint8_t i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
        servo[i] = DEFAULT_SERVO_MIDDLE;
    }
#ifdef USE_SERVOS
    initTailServoSymmetry();
#endif
}
#else
void mixerInit(mixerMode_e mixerMode, motorMixer_t *initialCustomMixers)
{
    currentMixerMode = mixerMode;
    customMixers = initialCustomMixers;
}
#endif

#ifdef USE_SERVOS
void mixerUsePWMIOConfiguration(pwmIOConfiguration_t *pwmIOConfiguration)
{
    int i;

    motorCount = 0;
    servoCount = pwmIOConfiguration->servoCount;

    if (currentMixerMode == MIXER_CUSTOM || currentMixerMode == MIXER_CUSTOM_TRI || currentMixerMode == MIXER_CUSTOM_AIRPLANE) {
        // load custom mixer into currentMixer
        for (i = 0; i < MAX_SUPPORTED_MOTORS; i++) {
            // check if done
            if (customMixers[i].throttle == 0.0f)
                break;
            currentMixer[i] = customMixers[i];
            motorCount++;
        }
    } else {
        motorCount = mixers[currentMixerMode].motorCount;
        // copy motor-based mixers
        if (mixers[currentMixerMode].motor) {
            for (i = 0; i < motorCount; i++)
                currentMixer[i] = mixers[currentMixerMode].motor[i];
        }
    }

    if (useServo) {
        servoRuleCount = servoMixers[currentMixerMode].servoRuleCount;
        if (servoMixers[currentMixerMode].rule) {
            for (i = 0; i < servoRuleCount; i++)
                currentServoMixer[i] = servoMixers[currentMixerMode].rule[i];
        }
    }
    
    // in 3D mode, mixer gain has to be halved
    if (feature(FEATURE_3D)) {
        if (motorCount > 1) {
            for (i = 0; i < motorCount; i++) {
                currentMixer[i].pitch *= 0.5f;
                currentMixer[i].roll *= 0.5f;
                currentMixer[i].yaw *= 0.5f;
            }
        }
    }

    // set flag that we're on something with wings
    if (currentMixerMode == MIXER_FLYING_WING ||
        currentMixerMode == MIXER_AIRPLANE ||
        currentMixerMode == MIXER_CUSTOM_AIRPLANE
    ) {
        ENABLE_STATE(FIXED_WING);
        
        if (currentMixerMode == MIXER_CUSTOM_AIRPLANE) {
            loadCustomServoMixer();
        }
    } else {
        DISABLE_STATE(FIXED_WING);

        if (currentMixerMode == MIXER_CUSTOM_TRI) {
            loadCustomServoMixer();
        }
    }

    mixerResetDisarmedMotors();
}
#else
void mixerUsePWMIOConfiguration(pwmIOConfiguration_t *pwmIOConfiguration)
{
    UNUSED(pwmIOConfiguration);
    motorCount = 4;
#ifdef USE_SERVOS
    servoCount = 0;
#endif
    uint8_t i;
    for (i = 0; i < motorCount; i++) {
        currentMixer[i] = mixerQuadX[i];
    }
    mixerResetDisarmedMotors();
}
#endif


#ifndef USE_QUAD_MIXER_ONLY
#ifdef USE_SERVOS
void loadCustomServoMixer(void)
{
    uint8_t i;

    // reset settings
    servoRuleCount = 0;
    memset(currentServoMixer, 0, sizeof(currentServoMixer));

    // load custom mixer into currentServoMixer
    for (i = 0; i < MAX_SERVO_RULES; i++) {
        // check if done
        if (customServoMixers[i].rate == 0)
            break;

        currentServoMixer[i] = customServoMixers[i];
        servoRuleCount++;
    }
}

void servoMixerLoadMix(int index, servoMixer_t *customServoMixers)
{
    int i;

    // we're 1-based
    index++;
    // clear existing
    for (i = 0; i < MAX_SERVO_RULES; i++)
        customServoMixers[i].targetChannel = customServoMixers[i].inputSource = customServoMixers[i].rate = customServoMixers[i].box = 0;

    for (i = 0; i < servoMixers[index].servoRuleCount; i++)
        customServoMixers[i] = servoMixers[index].rule[i];
}
#endif


void mixerLoadMix(int index, motorMixer_t *customMixers)
{
    int i;

    // we're 1-based
    index++;
    // clear existing
    for (i = 0; i < MAX_SUPPORTED_MOTORS; i++)
        customMixers[i].throttle = 0.0f;

    // do we have anything here to begin with?
    if (mixers[index].motor != NULL) {
        for (i = 0; i < mixers[index].motorCount; i++)
            customMixers[i] = mixers[index].motor[i];
    }
}

#endif

void mixerResetDisarmedMotors(void)
{
    int i;
    // set disarmed motor values
    for (i = 0; i < MAX_SUPPORTED_MOTORS; i++)
        motor_disarmed[i] = feature(FEATURE_3D) ? flight3DConfig->neutral3d : escAndServoConfig->mincommand;
}

#ifdef USE_SERVOS

STATIC_UNIT_TESTED void forwardAuxChannelsToServos(uint8_t firstServoIndex)
{
    // start forwarding from this channel
    uint8_t channelOffset = AUX1;

    uint8_t servoOffset;
    for (servoOffset = 0; servoOffset < MAX_AUX_CHANNEL_COUNT && channelOffset < MAX_SUPPORTED_RC_CHANNEL_COUNT; servoOffset++) {
        pwmWriteServo(firstServoIndex + servoOffset, rcData[channelOffset++]);
    }
}

static void updateGimbalServos(uint8_t firstServoIndex)
{
    pwmWriteServo(firstServoIndex + 0, servo[SERVO_GIMBAL_PITCH]);
    pwmWriteServo(firstServoIndex + 1, servo[SERVO_GIMBAL_ROLL]);
}

void writeServos(void)
{
    uint8_t servoIndex = 0;

    switch (currentMixerMode) {
        case MIXER_BICOPTER:
            pwmWriteServo(servoIndex++, servo[SERVO_BICOPTER_LEFT]);
            pwmWriteServo(servoIndex++, servo[SERVO_BICOPTER_RIGHT]);
            break;

        case MIXER_TRI:
        case MIXER_CUSTOM_TRI:
            if (mixerConfig->tri_unarmed_servo) {
                // if unarmed flag set, we always move servo
                pwmWriteServo(servoIndex++, servo[SERVO_RUDDER]);
            } else {
                // otherwise, only move servo when copter is armed
                if (ARMING_FLAG(ARMED))
                    pwmWriteServo(servoIndex++, servo[SERVO_RUDDER]);
                else
                    pwmWriteServo(servoIndex++, 0); // kill servo signal completely.
            }
            break;

        case MIXER_FLYING_WING:
            pwmWriteServo(servoIndex++, servo[SERVO_FLAPPERON_1]);
            pwmWriteServo(servoIndex++, servo[SERVO_FLAPPERON_2]);
            break;

        case MIXER_DUALCOPTER:
            pwmWriteServo(servoIndex++, servo[SERVO_DUALCOPTER_LEFT]);
            pwmWriteServo(servoIndex++, servo[SERVO_DUALCOPTER_RIGHT]);
            break;

        case MIXER_CUSTOM_AIRPLANE:
        case MIXER_AIRPLANE:
            for (int i = SERVO_PLANE_INDEX_MIN; i <= SERVO_PLANE_INDEX_MAX; i++) {
                pwmWriteServo(servoIndex++, servo[i]);
            }
            break;

        case MIXER_SINGLECOPTER:
            for (int i = SERVO_SINGLECOPTER_INDEX_MIN; i <= SERVO_SINGLECOPTER_INDEX_MAX; i++) {
                pwmWriteServo(servoIndex++, servo[i]);
            }
            break;

        default:
            break;
    }

    // Two servos for SERVO_TILT, if enabled
    if (feature(FEATURE_SERVO_TILT) || currentMixerMode == MIXER_GIMBAL) {
        updateGimbalServos(servoIndex);
        servoIndex += 2;
    }

    // forward AUX to remaining servo outputs (not constrained)
    if (feature(FEATURE_CHANNEL_FORWARDING)) {
        forwardAuxChannelsToServos(servoIndex);
        servoIndex += MAX_AUX_CHANNEL_COUNT;
    }
}
#endif

void writeMotors(void)
{
    uint8_t i;

    for (i = 0; i < motorCount; i++)
        pwmWriteMotor(i, motor[i]);


    if (feature(FEATURE_ONESHOT125)) {
        pwmCompleteOneshotMotorUpdate(motorCount);
    }
}

void writeAllMotors(int16_t mc)
{
    uint8_t i;

    // Sends commands to all motors
    for (i = 0; i < motorCount; i++)
        motor[i] = mc;
    writeMotors();
}

void stopMotors(void)
{
    writeAllMotors(feature(FEATURE_3D) ? flight3DConfig->neutral3d : escAndServoConfig->mincommand);

    delay(50); // give the timers and ESCs a chance to react.
}

void StopPwmAllMotors()
{
    pwmShutdownPulsesForAllMotors(motorCount);
}

#ifndef USE_QUAD_MIXER_ONLY
#ifdef USE_SERVOS
STATIC_UNIT_TESTED void servoMixer(void)
{
    int16_t input[INPUT_SOURCE_COUNT]; // Range [-500:+500]
    static int16_t currentOutput[MAX_SERVO_RULES];
    uint8_t i;

    if (FLIGHT_MODE(PASSTHRU_MODE)) {
        // Direct passthru from RX
        input[INPUT_STABILIZED_ROLL] = rcCommand[ROLL];
        input[INPUT_STABILIZED_PITCH] = rcCommand[PITCH];
        input[INPUT_STABILIZED_YAW] = rcCommand[YAW];
    } else {
        // Assisted modes (gyro only or gyro+acc according to AUX configuration in Gui
        input[INPUT_STABILIZED_ROLL] = axisPID[ROLL];
        input[INPUT_STABILIZED_PITCH] = axisPID[PITCH];
        input[INPUT_STABILIZED_YAW] = axisPID[YAW];

        // Reverse yaw servo when inverted in 3D mode
        if (feature(FEATURE_3D) && (rcData[THROTTLE] < rxConfig->midrc)) {
            input[INPUT_STABILIZED_YAW] *= -1;
        }
    }

    input[INPUT_GIMBAL_PITCH] = scaleRange(attitude.values.pitch, -1800, 1800, -500, +500);
    input[INPUT_GIMBAL_ROLL] = scaleRange(attitude.values.roll, -1800, 1800, -500, +500);

    input[INPUT_STABILIZED_THROTTLE] = motor[0] - 1000 - 500;  // Since it derives from rcCommand or mincommand and must be [-500:+500]

    // center the RC input value around the RC middle value
    // by subtracting the RC middle value from the RC input value, we get:
    // data - middle = input
    // 2000 - 1500 = +500
    // 1500 - 1500 = 0
    // 1000 - 1500 = -500
    input[INPUT_RC_ROLL]     = rcData[ROLL]     - rxConfig->midrc;
    input[INPUT_RC_PITCH]    = rcData[PITCH]    - rxConfig->midrc;
    input[INPUT_RC_YAW]      = rcData[YAW]      - rxConfig->midrc;
    input[INPUT_RC_THROTTLE] = rcData[THROTTLE] - rxConfig->midrc;
    input[INPUT_RC_AUX1]     = rcData[AUX1]     - rxConfig->midrc;
    input[INPUT_RC_AUX2]     = rcData[AUX2]     - rxConfig->midrc;
    input[INPUT_RC_AUX3]     = rcData[AUX3]     - rxConfig->midrc;
    input[INPUT_RC_AUX4]     = rcData[AUX4]     - rxConfig->midrc;

    for (i = 0; i < MAX_SUPPORTED_SERVOS; i++)
        servo[i] = 0;

    // mix servos according to rules
    for (i = 0; i < servoRuleCount; i++) {
        // consider rule if no box assigned or box is active
        if (currentServoMixer[i].box == 0 || IS_RC_MODE_ACTIVE(BOXSERVO1 + currentServoMixer[i].box - 1)) {
            uint8_t target = currentServoMixer[i].targetChannel;
            uint8_t from = currentServoMixer[i].inputSource;
            uint16_t servo_width = servoConf[target].max - servoConf[target].min;
            int16_t min = currentServoMixer[i].min * servo_width / 100 - servo_width / 2;
            int16_t max = currentServoMixer[i].max * servo_width / 100 - servo_width / 2;

            if (currentServoMixer[i].speed == 0)
                currentOutput[i] = input[from];
            else {
                if (currentOutput[i] < input[from])
                    currentOutput[i] = constrain(currentOutput[i] + currentServoMixer[i].speed, currentOutput[i], input[from]);
                else if (currentOutput[i] > input[from])
                    currentOutput[i] = constrain(currentOutput[i] - currentServoMixer[i].speed, input[from], currentOutput[i]);
            }

            servo[target] += servoDirection(target, from) * constrain(((int32_t)currentOutput[i] * currentServoMixer[i].rate) / 100, min, max);
        } else {
            currentOutput[i] = 0;
        }
    }

    for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
        servo[i] = ((int32_t)servoConf[i].rate * servo[i]) / 100L;
        servo[i] += determineServoMiddleOrForwardFromChannel(i);

    }

    static int16_t curveIndex = 0;
    static int8_t counter = 0;
    if (counter > 19)
    {
        curveIndex = (curveIndex + 1) % TRI_YAW_FORCE_CURVE_SIZE;
        counter = 0;
    }
    else
    {
        counter++;
    }

    if (currentMixerMode == MIXER_TRI)
    {
        if (ARMING_FLAG(ARMED))
        {
            servo[SERVO_RUDDER] = getLinearServoValue(&servoConf[0], servo[SERVO_RUDDER]);
        }

        virtualServoStep(dT, &servoConf[0], servo[SERVO_RUDDER]);
    }
}
#endif
#endif

uint16_t mixConstrainMotorForFailsafeCondition(uint8_t motorIndex)
{
    return constrain(motor[motorIndex], escAndServoConfig->mincommand, escAndServoConfig->maxthrottle);
}

void mixTable(void)
{
    uint32_t i;

    bool isFailsafeActive = failsafeIsActive();

    if (motorCount >= 4 && mixerConfig->yaw_jump_prevention_limit < YAW_JUMP_PREVENTION_LIMIT_HIGH) {
        // prevent "yaw jump" during yaw correction
        axisPID[YAW] = constrain(axisPID[YAW], -mixerConfig->yaw_jump_prevention_limit - ABS(rcCommand[YAW]), mixerConfig->yaw_jump_prevention_limit + ABS(rcCommand[YAW]));
    }

    if (currentMixerMode == MIXER_TRI)
    {
        // Adjust tail motor speed based on servo angle. Check how much to adjust speed from pitch force curve based on servo angle.
        // Take motor speed up lag into account by shifting the phase of the curve
        // Not taking into account the motor braking lag (yet)
        const float servoAngle_D = getVirtualServoAngleInDegrees();
        const float servoSetpointAngle_D = getServoAngleInDeciDegrees(&servoConf[0], servo[SERVO_RUDDER]) / 10.0f;

        float angleDiff_D = servoSetpointAngle_D - servoAngle_D;
        if (ABS(angleDiff_D) > TRI_TAIL_MOTOR_CURVE_MAX_PHASE_SHIFT_DEGREES)
        {
            angleDiff_D = TRI_TAIL_MOTOR_CURVE_MAX_PHASE_SHIFT_DEGREES * angleDiff_D / ABS(angleDiff_D);
        }

        const float futureServoAngle = constrainf(servoAngle_D + angleDiff_D, TRI_TAIL_SERVO_ANGLE_MID_DD - tailServoMaxAngle_DD, TRI_TAIL_SERVO_ANGLE_MID_DD + tailServoMaxAngle_DD);
        currentMixer[0].throttle = getPitchCorrectionAtTailAngle(futureServoAngle);
    }

    if (IS_RC_MODE_ACTIVE(BOXAIRMODE)) {
        // Initial mixer concept by bdoiron74 reused and optimized for Air Mode
        int16_t rollPitchYawMix[MAX_SUPPORTED_MOTORS];
        int16_t rollPitchYawMixMax = 0; // assumption: symetrical about zero.
        int16_t rollPitchYawMixMin = 0;

        // Find roll/pitch/yaw desired output
        for (i = 0; i < motorCount; i++) {
            rollPitchYawMix[i] =
                axisPID[PITCH] * currentMixer[i].pitch +
                axisPID[ROLL] * currentMixer[i].roll +
                -mixerConfig->yaw_motor_direction * axisPID[YAW] * currentMixer[i].yaw;

            if (rollPitchYawMix[i] > rollPitchYawMixMax) rollPitchYawMixMax = rollPitchYawMix[i];
            if (rollPitchYawMix[i] < rollPitchYawMixMin) rollPitchYawMixMin = rollPitchYawMix[i];
        }

        // Scale roll/pitch/yaw uniformly to fit within throttle range
        int16_t rollPitchYawMixRange = rollPitchYawMixMax - rollPitchYawMixMin;
        int16_t throttleRange, throttle;
        int16_t throttleMin, throttleMax;
        static int16_t throttlePrevious = 0;   // Store the last throttle direction for deadband transitions in 3D.

        // Find min and max throttle based on condition. Use rcData for 3D to prevent loss of power due to min_check
        if (feature(FEATURE_3D)) {
            if (!ARMING_FLAG(ARMED)) throttlePrevious = rxConfig->midrc; // When disarmed set to mid_rc. It always results in positive direction after arming.

            if ((rcData[THROTTLE] <= (rxConfig->midrc - flight3DConfig->deadband3d_throttle))) { // Out of band handling
                throttleMax = flight3DConfig->deadband3d_low;
                throttleMin = escAndServoConfig->minthrottle;
                throttlePrevious = throttle = rcData[THROTTLE];
            } else if (rcData[THROTTLE] >= (rxConfig->midrc + flight3DConfig->deadband3d_throttle)) { // Positive handling
                throttleMax = escAndServoConfig->maxthrottle;
                throttleMin = flight3DConfig->deadband3d_high;
                throttlePrevious = throttle = rcData[THROTTLE];
            } else if ((throttlePrevious <= (rxConfig->midrc - flight3DConfig->deadband3d_throttle)))  { // Deadband handling from negative to positive
                throttle = throttleMax = flight3DConfig->deadband3d_low;
                throttleMin = escAndServoConfig->minthrottle;
            } else {  // Deadband handling from positive to negative
                throttleMax = escAndServoConfig->maxthrottle;
                throttle = throttleMin = flight3DConfig->deadband3d_high;
            }
        } else {
            throttle = rcCommand[THROTTLE];
            throttleMin = escAndServoConfig->minthrottle;
            throttleMax = escAndServoConfig->maxthrottle;
        }

        throttleRange = throttleMax - throttleMin;

        if (rollPitchYawMixRange > throttleRange) {
            motorLimitReached = true;
            float mixReduction = (float) throttleRange / rollPitchYawMixRange;
            for (i = 0; i < motorCount; i++) {
                rollPitchYawMix[i] =  lrintf((float) rollPitchYawMix[i] * mixReduction);
            }
            // Get the maximum correction by setting throtte offset to center. Configurable limit will constrain values once limit exceeded to prevent spazzing out in crashes
            if (mixReduction > (mixerConfig->airmode_saturation_limit / 100.0f)) throttleMin = throttleMax = throttleMin + (throttleRange / 2);
        } else {
            motorLimitReached = false;
            throttleMin = throttleMin + (rollPitchYawMixRange / 2);
            throttleMax = throttleMax - (rollPitchYawMixRange / 2);
        }

        // Now add in the desired throttle, but keep in a range that doesn't clip adjusted
        // roll/pitch/yaw. This could move throttle down, but also up for those low throttle flips.
        for (i = 0; i < motorCount; i++) {
            motor[i] = rollPitchYawMix[i] + constrain(throttle * currentMixer[i].throttle, throttleMin, throttleMax);

            if (isFailsafeActive) {
                motor[i] = mixConstrainMotorForFailsafeCondition(i);
            } else if (feature(FEATURE_3D)) {
                if (throttlePrevious <= (rxConfig->midrc - flight3DConfig->deadband3d_throttle)) {
                    motor[i] = constrain(motor[i], escAndServoConfig->minthrottle, flight3DConfig->deadband3d_low);
                } else {
                    motor[i] = constrain(motor[i], flight3DConfig->deadband3d_high, escAndServoConfig->maxthrottle);
                }
            } else {
                motor[i] = constrain(motor[i], escAndServoConfig->minthrottle, escAndServoConfig->maxthrottle);
            }
        }
    } else {
        // motors for non-servo mixes
        for (i = 0; i < motorCount; i++) {
            motor[i] =
                rcCommand[THROTTLE] * currentMixer[i].throttle +
                axisPID[PITCH] * currentMixer[i].pitch +
                axisPID[ROLL] * currentMixer[i].roll +
                -mixerConfig->yaw_motor_direction * axisPID[YAW] * currentMixer[i].yaw;
        }

        // Find the maximum motor output.
        int16_t maxMotor = motor[0];
        for (i = 1; i < motorCount; i++) {
            // If one motor is above the maxthrottle threshold, we reduce the value
            // of all motors by the amount of overshoot.  That way, only one motor
            // is at max and the relative power of each motor is preserved.
            if (motor[i] > maxMotor) {
                maxMotor = motor[i];
            }
        }

        int16_t maxThrottleDifference = 0;
        if (maxMotor > escAndServoConfig->maxthrottle) {
            maxThrottleDifference = maxMotor - escAndServoConfig->maxthrottle;
        }

        for (i = 0; i < motorCount; i++) {
            // this is a way to still have good gyro corrections if at least one motor reaches its max.
            motor[i] -= maxThrottleDifference;

            if (feature(FEATURE_3D)) {
                if (mixerConfig->pid_at_min_throttle
                        || rcData[THROTTLE] <= rxConfig->midrc - flight3DConfig->deadband3d_throttle
                        || rcData[THROTTLE] >= rxConfig->midrc + flight3DConfig->deadband3d_throttle) {
                    if (rcData[THROTTLE] > rxConfig->midrc) {
                        motor[i] = constrain(motor[i], flight3DConfig->deadband3d_high, escAndServoConfig->maxthrottle);
                    } else {
                        motor[i] = constrain(motor[i], escAndServoConfig->mincommand, flight3DConfig->deadband3d_low);
                    }
                } else {
                    if (rcData[THROTTLE] > rxConfig->midrc) {
                        motor[i] = flight3DConfig->deadband3d_high;
                    } else {
                        motor[i] = flight3DConfig->deadband3d_low;
                    }
                }
            } else {
                if (isFailsafeActive) {
                    motor[i] = mixConstrainMotorForFailsafeCondition(i);
                } else {
                    // If we're at minimum throttle and FEATURE_MOTOR_STOP enabled,
                    // do not spin the motors.
                    motor[i] = constrain(motor[i], escAndServoConfig->minthrottle, escAndServoConfig->maxthrottle);
                    if ((rcData[THROTTLE]) < rxConfig->mincheck) {
                        if (feature(FEATURE_MOTOR_STOP)) {
                            motor[i] = escAndServoConfig->mincommand;
                        } else if (mixerConfig->pid_at_min_throttle == 0) {
                            motor[i] = escAndServoConfig->minthrottle;
                        }
                    }
                }
            }
        }
    }


    /* Disarmed for all mixers */
    if (!ARMING_FLAG(ARMED)) {
        for (i = 0; i < motorCount; i++) {
            motor[i] = motor_disarmed[i];
        }
    }

    // motor outputs are used as sources for servo mixing, so motors must be calculated before servos.

#if !defined(USE_QUAD_MIXER_ONLY) && defined(USE_SERVOS)

    // airplane / servo mixes
    switch (currentMixerMode) {
        case MIXER_CUSTOM_AIRPLANE:
        case MIXER_FLYING_WING:
        case MIXER_AIRPLANE:
        case MIXER_BICOPTER:
        case MIXER_CUSTOM_TRI:
        case MIXER_TRI:
        case MIXER_DUALCOPTER:
        case MIXER_SINGLECOPTER:
        case MIXER_GIMBAL:
            servoMixer();
            break;

        /*
        case MIXER_GIMBAL:
			servo[SERVO_GIMBAL_PITCH] = (((int32_t)servoConf[SERVO_GIMBAL_PITCH].rate * attitude.values.pitch) / 50) + determineServoMiddleOrForwardFromChannel(SERVO_GIMBAL_PITCH);
            servo[SERVO_GIMBAL_ROLL] = (((int32_t)servoConf[SERVO_GIMBAL_ROLL].rate * attitude.values.roll) / 50) + determineServoMiddleOrForwardFromChannel(SERVO_GIMBAL_ROLL);
            break;
        */

        default:
            break;
    }

    // camera stabilization
    if (feature(FEATURE_SERVO_TILT)) {
        // center at fixed position, or vary either pitch or roll by RC channel
        servo[SERVO_GIMBAL_PITCH] = determineServoMiddleOrForwardFromChannel(SERVO_GIMBAL_PITCH);
        servo[SERVO_GIMBAL_ROLL] = determineServoMiddleOrForwardFromChannel(SERVO_GIMBAL_ROLL);

        if (IS_RC_MODE_ACTIVE(BOXCAMSTAB)) {
            if (gimbalConfig->mode == GIMBAL_MODE_MIXTILT) {
                servo[SERVO_GIMBAL_PITCH] -= (-(int32_t)servoConf[SERVO_GIMBAL_PITCH].rate) * attitude.values.pitch / 50 - (int32_t)servoConf[SERVO_GIMBAL_ROLL].rate * attitude.values.roll / 50;
                servo[SERVO_GIMBAL_ROLL] += (-(int32_t)servoConf[SERVO_GIMBAL_PITCH].rate) * attitude.values.pitch / 50 + (int32_t)servoConf[SERVO_GIMBAL_ROLL].rate * attitude.values.roll / 50;
            } else {
                servo[SERVO_GIMBAL_PITCH] += (int32_t)servoConf[SERVO_GIMBAL_PITCH].rate * attitude.values.pitch / 50;
                servo[SERVO_GIMBAL_ROLL] += (int32_t)servoConf[SERVO_GIMBAL_ROLL].rate * attitude.values.roll  / 50;
            }
        }
    }

    // constrain servos
    for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
        servo[i] = constrain(servo[i], servoConf[i].min, servoConf[i].max); // limit the values
    }
#endif
}

#ifdef USE_SERVOS
bool isMixerUsingServos(void)
{
    return useServo;
}
#endif

void filterServos(void)
{
#ifdef USE_SERVOS
    int16_t servoIdx;

#if defined(MIXER_DEBUG)
    uint32_t startTime = micros();
#endif

    if (mixerConfig->servo_lowpass_enable) {
        for (servoIdx = 0; servoIdx < MAX_SUPPORTED_SERVOS; servoIdx++) {
            servo[servoIdx] = lrintf(applyBiQuadFilter((float) servo[servoIdx], &servoFilterState[servoIdx]));

            // Sanity check
            servo[servoIdx] = constrain(servo[servoIdx], servoConf[servoIdx].min, servoConf[servoIdx].max);
        }
    }
#if defined(MIXER_DEBUG)
    debug[0] = (int16_t)(micros() - startTime);
#endif

#endif
}

#ifdef USE_SERVOS

void initTailServoSymmetry()
{
    tailServoThrustFactor = mixerConfig->tri_tail_motor_thrustfactor / 10.0f;
    tailServoMaxAngle_DD = mixerConfig->tri_servo_angle_at_max;

    const int32_t minAngle_DD = TRI_TAIL_SERVO_ANGLE_MID_DD - tailServoMaxAngle_DD;
    const int32_t maxAngle_DD = TRI_TAIL_SERVO_ANGLE_MID_DD + tailServoMaxAngle_DD;
    int16_t maxNegForce = 0;
    int16_t maxPosForce = 0;

    int32_t angle_DD = TRI_TAIL_SERVO_ANGLE_MID_DD - TRI_TAIL_SERVO_MAX_ANGLE_DD;
    for (int32_t i = 0; i < TRI_YAW_FORCE_CURVE_SIZE; i++)
    {
        const float f_angle = angle_DD / 10.0f;
        yawForceCurve[i] = (uint16_t) 1000 * (-tailServoThrustFactor * cosf(DEGREES_TO_RADIANS(f_angle)) - sinf(DEGREES_TO_RADIANS(f_angle)) * getPitchCorrectionAtTailAngle(f_angle));
        // Only calculate the top forces in the configured angle range
        if ((angle_DD >= minAngle_DD) && (angle_DD < maxAngle_DD))
        {
            maxNegForce = MIN(yawForceCurve[i], maxNegForce);
            maxPosForce = MAX(yawForceCurve[i], maxPosForce);
        }
        angle_DD += 10;
    }

    tailServoMaxYawForce = MIN(ABS(maxNegForce), ABS(maxPosForce));
}

uint16_t getServoValueAtAngle(servoParam_t *servoConf, uint16_t angle_DD)
{
    int16_t servoMid = servoConf->middle;
    uint16_t servoValue;

    if (angle_DD < TRI_TAIL_SERVO_ANGLE_MID_DD)
    {
        int16_t servoMin = servoConf->min;
        servoValue = (angle_DD - tailServoMaxAngle_DD) * INT_PRECISION /
                    (TRI_TAIL_SERVO_ANGLE_MID_DD - tailServoMaxAngle_DD) *
                    (servoMid - servoMin) / INT_PRECISION;
        servoValue += servoMin;
    }
    else if (angle_DD > TRI_TAIL_SERVO_ANGLE_MID_DD)
    {
        int16_t servoMax = servoConf->max;

        servoValue = (angle_DD - TRI_TAIL_SERVO_ANGLE_MID_DD) * INT_PRECISION / tailServoMaxAngle_DD *
                            (servoMax - servoMid) / INT_PRECISION;
        servoValue += servoMid;
    }
    else
    {
        servoValue = servoMid;
    }

    return servoValue;
}

float getPitchCorrectionAtTailAngle(float angle)
{
    return 1 / (sin_approx(DEGREES_TO_RADIANS(angle)) - cos_approx(DEGREES_TO_RADIANS(angle)) / tailServoThrustFactor);
}

uint16_t getAngleFromYawCurveAtForce(int16_t force)
{
    if (force < yawForceCurve[0]) // No force that low
    {
        return TRI_TAIL_SERVO_ANGLE_MID_DD - TRI_TAIL_SERVO_MAX_ANGLE_DD;
    }
    else if (!(force < yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE - 1])) // No force that high
    {
        return TRI_TAIL_SERVO_ANGLE_MID_DD + TRI_TAIL_SERVO_MAX_ANGLE_DD;
    }
    // Binary search: yawForceCurve[lower] <= force, yawForceCurve[higher] > force
    int32_t lower = 0, higher = TRI_YAW_FORCE_CURVE_SIZE - 1;
    while (higher > lower + 1)
    {
        const int32_t mid = (lower + higher) / 2;
        if (yawForceCurve[mid] > force)
        {
            higher = mid;
        }
        else
        {
            lower = mid;
        }
    }
    // Interpolating
    return TRI_TAIL_SERVO_ANGLE_MID_DD - TRI_TAIL_SERVO_MAX_ANGLE_DD + lower * 10 + (force - yawForceCurve[lower]) * 1000 / (yawForceCurve[higher] - yawForceCurve[lower]) / 100;
}

uint16_t getLinearServoValue(servoParam_t *servoConf, uint16_t servoValue)
{
    const int16_t servoMid = servoConf->middle;
    // First find the yaw force at given servo value from a linear curve
    const int32_t range = (servoValue < servoMid) ? servoMid - servoConf->min : servoConf->max - servoMid;
    const int32_t linearYawForceAtValue = (servoValue - servoMid) * INT_PRECISION / range * tailServoMaxYawForce / INT_PRECISION;
    const int16_t correctedAngle_DD = getAngleFromYawCurveAtForce(linearYawForceAtValue);
    return getServoValueAtAngle(servoConf, correctedAngle_DD);
}

uint16_t getServoAngleInDeciDegrees(servoParam_t *servoConf, uint16_t servoValue)
{
    const int16_t midValue = servoConf->middle;
    const int16_t endValue = servoValue < midValue ? servoConf->min : servoConf->max;
    const int16_t endAngle = servoValue < midValue ? TRI_TAIL_SERVO_ANGLE_MID_DD - tailServoMaxAngle_DD : TRI_TAIL_SERVO_ANGLE_MID_DD + tailServoMaxAngle_DD;
    const int16_t servoAngle = (endAngle - TRI_TAIL_SERVO_ANGLE_MID_DD) * (servoValue - midValue) * INT_PRECISION / (endValue - midValue) / INT_PRECISION + TRI_TAIL_SERVO_ANGLE_MID_DD;
    return servoAngle;
}

void virtualServoStep(float dT, servoParam_t *servoConf, uint16_t servoValue)
{
    const float angleSetPoint = getServoAngleInDeciDegrees(servoConf, servoValue) / 10.0f;
    const float dA = dT * mixerConfig->tri_tail_servo_speed; // Max change of an angle since last check
    if ( ABS(virtualServoAngle_D - angleSetPoint) < dA )
    {
        // At set-point after this moment
        virtualServoAngle_D = angleSetPoint;
    }
    else if (virtualServoAngle_D < angleSetPoint)
    {
        virtualServoAngle_D += dA;
    }
    else // virtualServoAngle > angleSetPoint
    {
        virtualServoAngle_D -= dA;
    }
}

float getVirtualServoAngleInDegrees()
{
    return virtualServoAngle_D;
}
#endif // USE_SERVOS
