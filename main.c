#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/cm3/nvic.h>
#include <stdio.h>
#include <string.h>

#include "pwm.h"
#include "usb_cdc.h"
#include "spi.h"

#include "systick.h"

#define TRACE() printf("%s:%d\r\n", __func__, __LINE__)

static void setup_gpio(void) {
	RCC_APB2ENR |= RCC_APB2ENR_IOPCEN;
	GPIOC_CRH = (GPIO_CNF_OUTPUT_PUSHPULL << (((13 - 8) * 4) + 2));
	GPIOC_CRH |= (GPIO_MODE_OUTPUT_2_MHZ << ((13 - 8) * 4));
}

int main(void)
{
	rcc_clock_setup_in_hse_8mhz_out_72mhz();
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_TIM2);
	rcc_periph_clock_enable(RCC_AFIO);
	rcc_periph_clock_enable(RCC_SPI1);
	rcc_periph_clock_enable(RCC_DMA1);

	systick_init();
	setup_gpio();

	usb_cdc_init();
	spi_init();
	spi_slave_enable(SPI1);

	pwm_timer_init(TIM2, 200000);
	pwm_timer_enable(TIM2);

	pwm_channel_set_duty(TIM2, TIM_OC1, 0x8000);
	pwm_channel_enable(TIM2, TIM_OC1);
	pwm_channel_set_duty(TIM2, TIM_OC2, 0x4000);
	pwm_channel_enable(TIM2, TIM_OC2);
	pwm_channel_set_duty(TIM2, TIM_OC3, 0x2000);
	pwm_channel_enable(TIM2, TIM_OC3);
	pwm_channel_set_duty(TIM2, TIM_OC4, 0x1000);
	pwm_channel_enable(TIM2, TIM_OC4);

	pwm_channel_set_duty(TIM2, TIM_OC1, 10000);

	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL,
		      GPIO14 | GPIO15);
	gpio_set(GPIOC, GPIO14);
	gpio_clear(GPIOC, GPIO15);

	gpio_set(GPIOC, GPIO13);

	while (!usb_usart_dtr());
	printf("\r\nStandard I/O Example.\r\n");

	spi_dump_lists();

	struct spi_pl_packet *pkt;
	while (1) {
		delay_ms(10);
		spi_dump_trace();
		while ((pkt = spi_receive_packet())) {
			spi_send_packet(pkt);
		}
	}

	return 0;
}
