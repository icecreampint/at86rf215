#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/printk.h>
/* #include <linux/platform_device.h> */
/* #include <linux/regmap.h> */
/* #include <linux/spi/at86rf230.h> */
#include <linux/spi/spi.h>
/* #include <net/mac802154.h> */

static struct timer_list ping_timer;

/* RSTN pin number */
static int rstn_pin_num;

static int at86rf215_cookie = 0xbabef00d;

/* Retrieve RSTN pin number from device tree */
static int at86rf215_get_platform_data(struct spi_device *spi, int *rstn_pin_num)
{
	if (!spi->dev.of_node) {
		dev_err(&spi->dev, "no device tree node found!\n");
		return -ENOENT;
	}

	*rstn_pin_num = of_get_named_gpio(spi->dev.of_node, "reset-gpio", 0);

	return 0;
}

static irqreturn_t at86rf215_irq_handler(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void ping_process(struct timer_list *t)
{
	pr_debug("ping\n");
	mod_timer(&ping_timer, jiffies + msecs_to_jiffies(5000));
}

static int at86rf215_probe(struct spi_device *spi)
{
	unsigned long flags;

	if (!spi->irq) {
		dev_err(&spi->dev, "no IRQ number\n");
		return -1;
	}

	dev_dbg(&spi->dev, "IRQ number is %d\n", spi->irq);

	/* Retrieve RSTN pin number */ 
	if (at86rf215_get_platform_data(spi, &rstn_pin_num) < 0 ||
		!gpio_is_valid(rstn_pin_num)) {
		return -1;
	}

	dev_dbg(&spi->dev, "RSTN pin number is %d\n", rstn_pin_num);

	/* Configure as output, active low */
	if (devm_gpio_request_one(&spi->dev, rstn_pin_num,
				  GPIOF_OUT_INIT_HIGH, "rstn") < 0) {
		return -1;
	}

	/* TODO: read IRQ status reg to reset line */
	flags = irq_get_trigger_type(spi->irq);
	if (!flags)
		flags = IRQF_TRIGGER_HIGH;

	if (devm_request_irq(&spi->dev, spi->irq, at86rf215_irq_handler,
			     IRQF_SHARED | flags,
			     dev_name(&spi->dev), &at86rf215_cookie) < 0) {
		devm_gpio_free(&spi->dev, rstn_pin_num);
		return -1;
	}

	disable_irq(spi->irq);

	timer_setup(&ping_timer, ping_process, 0);
	
	ping_process(NULL);

	return 0;
}

static int at86rf215_remove(struct spi_device *spi)
{
	devm_free_irq(&spi->dev, spi->irq, &at86rf215_cookie);
	devm_gpio_free(&spi->dev, rstn_pin_num);

	return 0;
}

static const struct of_device_id at86rf215_of_match[] = {
	{ .compatible = "atmel,at86rf233", },
	{ },
};
MODULE_DEVICE_TABLE(of, at86rf215_of_match);

static const struct spi_device_id at86rf215_device_id[] = {
	{ .name = "at86rf233", },
	{ },
};
MODULE_DEVICE_TABLE(spi, at86rf215_device_id);

static struct spi_driver at86rf215_driver = {
	.id_table = at86rf215_device_id,
	.driver = {
		.of_match_table = of_match_ptr(at86rf215_of_match),
		.name = "at86rf233"
	},
	.probe = at86rf215_probe,
	.remove = at86rf215_remove
};

module_spi_driver(at86rf215_driver);

MODULE_DESCRIPTION("Atmel AT86RF215 radio transceiver driver");
MODULE_LICENSE("GPL v2");
