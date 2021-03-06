/*
 * Copyright (C) 2017 Brian Starkey <stark3y@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libopencm3/stm32/timer.h>
#include <libopencm3/cm3/nvic.h>
#include <stdio.h>
#include <stdlib.h>

#include "motor.h"
#include "hbridge.h"
#include "spi.h"
#include "period_counter.h"
#include "controller.h"

#include "systick.h"

struct motor {
	struct controller controller;
	uint32_t duty;
	uint32_t period;
	uint32_t count;
	uint32_t setpoint;
	enum hbridge_channel channel;
	enum pc_channel pc_channel;
	enum direction dir;
	uint8_t enabling;

	int changing_direction :1;
};

struct motor_data {
	uint32_t timestamp;
	uint8_t channel;
	uint8_t direction;
	uint16_t duty;
	uint32_t period;
	int32_t count;
};

struct motor motors[] = {
	[HBRIDGE_A] = {
		.channel = HBRIDGE_A,
		.pc_channel = PC_CH1,
	},
	[HBRIDGE_B] = {
		.channel = HBRIDGE_B,
		.pc_channel = PC_CH2,
	},
};

const struct gain gains[] = {
	{ FP_VAL(-130), 0, 0 },
	{ FP_VAL(-50), 0, 0 },
	{ FP_VAL(-10), 0, 0 },
	{ FP_VAL(-5), 0, 0 },
	{ FP_VAL(-2), 0, 0 },
	{ FP_VAL(-1), 0, 0 },
	/*
	{ FP_VAL(-1), 0, 0 },
	{ FP_VAL(-3), 0, 0 },
	{ FP_VAL(-5), 0, 0 },
	{ FP_VAL(-8), 0, 0 },
	{ FP_VAL(-25), 0, 0 },
	{ FP_VAL(-60), 0, 0 },
	{ FP_VAL(-100), 0, 0 },

	{ FP_VAL(-130), 0, 0 },
	{ FP_VAL(-200), 0, 0 },
	*/
};

const uint16_t gs_limits[] = {
	100,
	200,
	400,
	700,
	800,
	10000,
	/*
	4000,
	5000,
	6000,
	10000,
	15000,
	20000,
	25000,

	30000,
	65535,
	*/
};

struct period_counter pc = {
	.timer = TIM4,
};

struct hbridge hb = {
	.timer = TIM2,
	.a = {
		.ch1 = TIM_OC1,
		.ch2 = TIM_OC2,
	},
	.b = {
		.ch1 = TIM_OC3,
		.ch2 = TIM_OC4,
	},
};

static void pid_timer_init(uint32_t timer)
{
	timer_reset(timer);
	timer_slave_set_mode(timer, TIM_SMCR_SMS_OFF);
	timer_set_prescaler(timer, 7100);
	timer_set_mode(timer, TIM_CR1_CKD_CK_INT,
		       TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
	timer_enable_preload(timer);
	timer_update_on_overflow(timer);
	timer_enable_update_event(timer);
	timer_generate_event(timer, TIM_EGR_UG);

	timer_enable_irq(timer, TIM_DIER_UIE);
	nvic_enable_irq(NVIC_TIM3_IRQ);
	timer_set_period(timer, 500);
}

static void pid_timer_enable(uint32_t timer)
{
	timer_enable_counter(timer);
}

static void pid_timer_disable(uint32_t timer)
{
	timer_disable_counter(timer);
}

void motor_set_speed(enum hbridge_channel channel, enum direction dir,
		     uint16_t speed)
{
	struct motor *m = &motors[channel];

	if (speed == 0) {
		period_counter_disable(&pc, m->pc_channel);

		if (m->setpoint != 0) {
			struct spi_pl_packet *pkt = spi_alloc_packet();
			if (pkt) {
				struct motor_data *d = (struct motor_data *)pkt->data;
				pkt->type = 15;
				d->timestamp = msTicks;
				d->channel = m->channel;
				d->direction = m->dir;
				d->duty = 0;
				d->period = 0;
				d->count = m->count;

				spi_send_packet(pkt);
			}
		}

		m->dir = dir;
		m->setpoint = 0;
		controller_set(&m->controller, 0);
		return;
	} else if (m->setpoint == 0) {
		m->enabling = 10;
		period_counter_enable(&pc, m->pc_channel);
	}

	if (dir != m->dir) {
		m->changing_direction = 1;
	}

	m->dir = dir;
	m->setpoint = speed;
	controller_set(&m->controller, speed);
}

void motor_disable_loop()
{
	pid_timer_disable(TIM3);
	period_counter_disable(&pc, PC_CH1);
	period_counter_disable(&pc, PC_CH2);
}

void motor_enable_loop()
{
	motor_disable_loop();
	controller_reset(&motors[HBRIDGE_A].controller);
	controller_reset(&motors[HBRIDGE_B].controller);
	period_counter_enable(&pc, PC_CH1);
	period_counter_enable(&pc, PC_CH2);
	pid_timer_enable(TIM3);
}

static uint8_t gain_schedule(uint32_t point)
{
	uint8_t gs_idx;
	for (gs_idx = 0; gs_idx < sizeof(gains) / sizeof(gains[0]); gs_idx++) {
		if (point <= gs_limits[gs_idx])
			break;
	}
	return gs_idx;
}

static void motor_tick(struct motor *m)
{
	int32_t delta;
	int32_t count;
	uint8_t gs_idx;

	if (m->setpoint == 0) {
		m->duty = 0;
		hbridge_set_duty(&hb, m->channel, m->dir, 0);
		return;
	}

	if (m->changing_direction) {
		hbridge_set_duty(&hb, m->channel, m->dir, 0);
		m->changing_direction = 0;
		return;
	}

	m->period = period_counter_get(&pc, m->pc_channel);
	if (m->enabling && m->period != 0) {
		m->period = 0;
		m->enabling = 0;
	}

	count = period_counter_get_total(&pc, m->pc_channel);
	period_counter_reset_total(&pc, m->pc_channel);
	if (m->dir== DIRECTION_FWD) {
		m->count += count;
	} else if (m->dir == DIRECTION_REV) {
		m->count -= count;
	}

	uint64_t mid = (m->period + m->setpoint) >> 1;

	gs_idx = gain_schedule(mid);
	delta = controller_tick(&m->controller, m->period, gs_idx);
	if (!delta)
		return;

	if ((delta < 0) && (-delta > (int32_t)m->duty)) {
		m->duty = 0;
	} else if (m->duty + delta > 0xffff) {
		m->duty = 0xffff;
	} else {
		m->duty += delta;
	}

	if (m->duty < 3000) {
		m->duty = 3000;
	}
	hbridge_set_duty(&hb, m->channel, m->dir, m->duty);

	struct spi_pl_packet *pkt = spi_alloc_packet();
	if (pkt) {
		struct motor_data *d = (struct motor_data *)pkt->data;
		pkt->type = 15;
		d->timestamp = msTicks;
		d->channel = m->channel;
		d->direction = m->dir;
		d->duty = m->duty;
		d->period = m->period;
		d->count = m->count;

		spi_send_packet(pkt);
	}
}

void tim3_isr(void)
{
	timer_clear_flag(TIM3, TIM_SR_UIF);
	motor_tick(&motors[HBRIDGE_A]);
	motor_tick(&motors[HBRIDGE_B]);
}

void tim4_isr(void)
{
	period_counter_update(&pc);
}

enum motor_type {
	MOTOR_SET = 0,
};

struct motor_cmd_set {
	struct {
		enum direction dir;
		uint32_t setpoint;
	} motors[2];
};

struct motor_cmd {
	enum motor_type type;
	union {
		/* type == MOTOR_SET */
		struct motor_cmd_set set;
	} payloads;
};

void motor_process_packet(struct spi_pl_packet *pkt)
{
	struct motor_cmd *cmd = (struct motor_cmd *)pkt->data;

	if (cmd->type == MOTOR_SET) {
		struct motor_cmd_set *set = &cmd->payloads.set;
		motor_set_speed(HBRIDGE_A, set->motors[0].dir, set->motors[0].setpoint);
		motor_set_speed(HBRIDGE_B, set->motors[1].dir, set->motors[1].setpoint);
	}
}

void motor_init()
{
	hbridge_init(&hb);
	period_counter_init(&pc);
	pid_timer_init(TIM3);

	controller_init(&motors[HBRIDGE_A].controller, gains, sizeof(gains) / sizeof(gains[0]));
	controller_init(&motors[HBRIDGE_B].controller, gains, sizeof(gains) / sizeof(gains[0]));
}
