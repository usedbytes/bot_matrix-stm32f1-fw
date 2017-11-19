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
#include <stdio.h>
#include <stdbool.h>

#include "controller.h"

char dbg[256];
volatile bool cp;

void controller_init(struct controller *c, const struct gain *gs, uint8_t ngains) {
	c->skip = 1;
	c->gains = gs;
	c->ngains = ngains;
}

void controller_set_gains(struct controller *c, int32_t Kc, int32_t Kd, int32_t Ki) {
	if (Kc || Kd || Ki) {
		c->gain.Kc = Kc;
		c->gain.Kd = Kd;
		c->gain.Ki = Ki;
		c->gain_override = true;
	} else {
		c->gain_override = false;
	}
}

void controller_set(struct controller *c, uint32_t set_point) {
	c->set_point = set_point;
}

uint32_t controller_get(struct controller *c) {
	return c->set_point;
}

void controller_set_ilimit(struct controller *c, int32_t ilimit) {
	c->ilimit = ilimit;
}

int32_t controller_tick(struct controller *c, uint32_t pv, uint8_t gs_idx) {
	const struct gain *gains;
	int32_t tc, td, ti, ret;
	int32_t err, derr;

	if (pv == 0 || gs_idx >= c->ngains) {
		c->skip++;
		return 0;
	}

	gains = c->gain_override ? &c->gain : &c->gains[gs_idx];

	err = c->set_point - pv;
	c->ierr += ((err * 25) / (256 * c->skip));
	if (c->ierr > c->ilimit) {
		c->ierr = c->ilimit;
	} else if (c->ierr < -c->ilimit) {
		c->ierr = -c->ilimit;
	}
	derr = (err - c->err) * (10 * c->skip);
	c->err = err;
	c->skip = 1;

	tc = ((int64_t)(err) * gains->Kc) / 65536;
	td = ((int64_t)(derr) * gains->Kd) / 65536;
	ti = ((int64_t)(c->ierr) * gains->Ki) / 65536;
	ret = tc + td + ti;

	sprintf(dbg, "idx: %d, Kc: %ld, Kd: %ld, Ki: %ld\r\n"
		"err: %li, derr: %li, ierr: %li\r\n"
		"tc: %li, td: %li, ti: %li ret: %li\r\n", gs_idx, gains->Kc, gains->Kd, gains->Ki, err, derr, c->ierr, tc, td, ti, ret);
	cp = true;

	return ret;
}
