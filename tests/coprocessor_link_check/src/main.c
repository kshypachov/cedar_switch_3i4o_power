/* Soldering check for a blank ESP32-C6.
 *
 * A chip straight out of the blister has empty flash, so its ROM prints a
 * banner and then complains about the invalid image header - that alone proves
 * the module is powered, released from reset and its TX reaches the STM32.
 * Download mode then proves the boot strap and our RX direction.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>

static const struct gpio_dt_spec rst =
	GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_reset), gpios);
static const struct gpio_dt_spec bootp =
	GPIO_DT_SPEC_GET(DT_NODELABEL(wifi_boot), gpios);
static const struct device *const gpioc = DEVICE_DT_GET(DT_NODELABEL(gpioc));
static const struct device *const u3 = DEVICE_DT_GET(DT_NODELABEL(usart3));

#define TX_PIN 10 /* PC10: STM32 -> C6 */
#define RX_PIN 11 /* PC11: C6 -> STM32 */

static void line_probe(void)
{
	int pu, pd;

	gpio_pin_configure(gpioc, RX_PIN, GPIO_INPUT | GPIO_PULL_UP);
	k_msleep(5);
	pu = gpio_pin_get(gpioc, RX_PIN);
	gpio_pin_configure(gpioc, RX_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
	k_msleep(5);
	pd = gpio_pin_get(gpioc, RX_PIN);
	printk("C6 TX line (PC11): pull-up=%d pull-down=%d -> %s\n", pu, pd,
	       (pu == 1 && pd == 1) ? "driven HIGH (idle UART, module alive)"
	       : (pu == 0 && pd == 0) ? "driven LOW"
	       : "floating - nothing drives it");
}

static int dump(const char *tag, int ms)
{
	unsigned char c;
	int n = 0;
	int64_t end = k_uptime_get() + ms;

	printk("--- %s ---\n", tag);
	while (k_uptime_get() < end) {
		if (uart_poll_in(u3, &c) == 0) {
			if (c == '\n' || (c >= 0x20 && c < 0x7f)) {
				printk("%c", c);
			} else if (c != '\r') {
				printk("<%02x>", c);
			}
			n++;
		}
	}
	printk("\n--- %s: %d bytes ---\n", tag, n);
	return n;
}

int main(void)
{
	int normal, download;

	printk("\n=== ESP32-C6 soldering check ===\n");
	printk("usart3 ready=%d gpioc ready=%d\n", device_is_ready(u3),
	       device_is_ready(gpioc));

	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&bootp, GPIO_OUTPUT_INACTIVE);
	k_msleep(50);

	gpio_pin_set_dt(&rst, 1);
	k_msleep(100);
	gpio_pin_set_dt(&rst, 0);
	normal = dump("normal boot (expect ROM banner + invalid header)", 2500);

	gpio_pin_set_dt(&rst, 1);
	gpio_pin_set_dt(&bootp, 1);
	k_msleep(100);
	gpio_pin_set_dt(&rst, 0);
	k_msleep(200);
	gpio_pin_set_dt(&bootp, 0);
	download = dump("download mode (expect 'waiting for download')", 2500);

	/* Done with the UART; now look at the raw line level. This steals PC11
	 * from USART3, so it has to come last. */
	line_probe();

	printk("\nRESULT: normal=%d bytes, download=%d bytes -> %s\n",
	       normal, download,
	       (normal > 0 || download > 0) ? "MODULE RESPONDS" : "NO RESPONSE");
	return 0;
}
