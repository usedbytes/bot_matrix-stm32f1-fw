#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "hbridge.h"
#include "log.h"
#include "motor.h"
#include "pwm.h"
#include "spi.h"
#include "usb_cdc.h"

#include "systick.h"

#define ARRAY_SIZE(_x) ((sizeof(_x) / sizeof(_x[0])))

static void setup_irq_priorities(void)
{
	struct map_entry {
		uint32_t irqn;
		uint8_t  prio;
	} map[] = {
		{ NVIC_EXTI4_IRQ,           (0 << 6) | (0 << 4) },
		{ NVIC_TIM4_IRQ,            (1 << 6) | (0 << 4) },
		{ NVIC_USB_LP_CAN_RX0_IRQ,  (2 << 6) | (0 << 4) },
		{ NVIC_USB_WAKEUP_IRQ,      (2 << 6) | (1 << 4) },
		{ NVIC_TIM3_IRQ,            (3 << 6) | (0 << 4) },
		{ 0, 0 }
	}, *p = map;

	/*
	 * Priority ordering to try and make SPI reliable...
	 * stm32f103 only implements 4 bits of priority!
	 * Interrupt priority grouping (2 bits of pre-empt):
	 *   7:6 - pre-emption
	 *   5:4 - priority
	 *   3:0 - unused
	 */
	scb_set_priority_grouping(5 << 8);
	while (p->irqn) {
		nvic_set_priority(p->irqn, p->prio);
		p++;
	};
}

static void setup_gpio(void) {
	RCC_APB2ENR |= RCC_APB2ENR_IOPCEN;
	GPIOC_CRH = (GPIO_CNF_OUTPUT_PUSHPULL << (((13 - 8) * 4) + 2));
	GPIOC_CRH |= (GPIO_MODE_OUTPUT_2_MHZ << ((13 - 8) * 4));
}

struct time_sync {
	uint32_t cookie;
	uint32_t board_millis;
	int64_t real_nanos;
};

static void time_sync_process_packet(struct spi_pl_packet *pkt)
{
	struct time_sync *sync = (struct time_sync *)pkt->data;
	if ((pkt->type != 0x1) || (pkt->flags & SPI_FLAG_ERROR))
		return;

	sync->board_millis = msTicks;
}

static void ep0xfe_process_packet(struct spi_pl_packet *pkt)
{
	if ((pkt->type != 0xfe) || (pkt->flags & SPI_FLAG_ERROR))
		return;

	scb_reset_system();
}

#define EP_GPIO 19
struct gpio_set_cmd {
	uint8_t port;
	uint8_t pin;
	uint8_t state;
};

static void gpio_set_process_packet(struct spi_pl_packet *pkt)
{
	const uint32_t ports[] = { GPIOA, GPIOB, GPIOC };
	struct gpio_set_cmd *cmd = (struct gpio_set_cmd *)pkt->data;

	if ((pkt->type != EP_GPIO) || (pkt->flags & SPI_FLAG_ERROR))
		return;

	if (cmd->port >= ARRAY_SIZE(ports) || cmd->pin > 15) {
		log_err("GPIO out of range (Port %d, pin %d)\n", cmd->port, cmd->pin);
		return;
	}

	if (cmd->state) {
		gpio_set(ports[cmd->port], (1 << cmd->pin));
	} else {
		gpio_clear(ports[cmd->port], (1 << cmd->pin));
	}
}

#define EP_MOTORS 18

int main(void)
{
	rcc_clock_setup_in_hse_8mhz_out_72mhz();
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_TIM2);
	rcc_periph_clock_enable(RCC_TIM3);
	rcc_periph_clock_enable(RCC_TIM4);
	rcc_periph_clock_enable(RCC_AFIO);
	rcc_periph_clock_enable(RCC_SPI1);
	rcc_periph_clock_enable(RCC_DMA1);

	systick_init();
	setup_gpio();

	spi_init();
	spi_slave_enable(SPI1);

	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL,
		      GPIO14 | GPIO15);
	gpio_set(GPIOC, GPIO14);
	gpio_clear(GPIOC, GPIO15);

	gpio_set(GPIOC, GPIO13);

	motor_init();
	motor_enable_loop();
	motor_set_speed(HBRIDGE_A, DIRECTION_FWD, 0);
	motor_set_speed(HBRIDGE_B, DIRECTION_FWD, 0);

	setup_irq_priorities();

	struct spi_pl_packet *pkt;
	uint32_t time = msTicks;
	while (1) {
		while ((pkt = spi_receive_packet())) {
#ifdef DEBUG
			spi_dump_packet("", pkt);
#endif
			if (pkt->flags & SPI_FLAG_CRCERR) {
				log_err("CRC error in packet id %d\n", (uint32_t)pkt->id);
				spi_free_packet(pkt);
				continue;
			}
			switch (pkt->type) {
				case 0x1:
					time_sync_process_packet(pkt);
					/* Bounce it back */
					spi_send_packet(pkt);
					break;
				case EP_MOTORS:
					motor_process_packet(pkt);
					spi_free_packet(pkt);
					break;
				case EP_GPIO:
					gpio_set_process_packet(pkt);
					spi_free_packet(pkt);
					break;
				case 0xfe:
					ep0xfe_process_packet(pkt);
					spi_free_packet(pkt);
					break;
				default:
					// Bounce anything unknown
					spi_send_packet(pkt);
			}
		}

		if (msTicks - time >= 100) {
			time = msTicks;
			// Do something periodically...
		}
	}

	return 0;
}
