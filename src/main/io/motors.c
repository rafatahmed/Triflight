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
#include <string.h>

#include <platform.h>

#include "build/debug.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"
#include "config/config_reset.h"

#include "motors.h"

#define BRUSHED_MOTORS_PWM_RATE 16000
#define BRUSHLESS_MOTORS_PWM_RATE 400

#ifdef BRUSHED_MOTORS
#define DEFAULT_PWM_RATE BRUSHED_MOTORS_PWM_RATE
#else
#define DEFAULT_PWM_RATE BRUSHLESS_MOTORS_PWM_RATE
#endif

PG_REGISTER_WITH_RESET_TEMPLATE(motorConfig_t, motorConfig, PG_MOTOR_CONFIG, 1);

PG_RESET_TEMPLATE(motorConfig_t, motorConfig,
    .minthrottle = 1100,
    .maxthrottle = 2000,
    .mincommand = 1000,
    .reserved1 = 0,
    .motor_pwm_rate = DEFAULT_PWM_RATE,
    .reserved2 = 0,
);
