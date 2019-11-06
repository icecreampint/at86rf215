#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/printk.h>
/* #include <linux/platform_device.h> */
#include <linux/regmap.h>
/* #include <linux/spi/at86rf230.h> */
#include <linux/spi/spi.h>
/* #include <net/mac802154.h> */

#include "reg.h"

static bool is_configured;

enum { AT86RF215 = 0x34, AT86RF215IQ, AT86RF215M };

static const struct trx_id {
	unsigned char part_number;
	const char *description;
} trx_ids[] = {
	{ AT86RF215, "AT86RF215" },
	{ AT86RF215IQ, "AT86RF215IQ" },
	{ AT86RF215M, "AT86RF215M" },
};
#define num_of_trx_ids (sizeof(trx_ids) / sizeof(trx_ids[0]))

enum { RF_DUMMY1        = 0x0,
       RF_DUMMY2,
       RF_TRXOFF        = 0x2,  /* Transceiver off, SPI active */
       RF_TXPREP,               /* Transmit preparation */
       RF_TX,                   /* Transmit */
       RF_RX,                   /* Receive */
       RF_TRANSITION,           /* State transition in progress */
       RF_RESET,                /* Transceiver is in state RESET or SLEEP */
       RF_MAX };

static const char *trx_state_descs[RF_MAX] = {
	"DUMMY1", "DUMMY2", "TRXOFF", "TXPREP", "TX", "RX", "TRANSITION", "RESET"
};

static const struct trx_state {
	unsigned char value;
	const char *description;
} trx_states[] = {
	{ RF_TRXOFF, "TRXOFF" }, { RF_TXPREP, "TXPREP" },         { RF_TX, "TX" },
	{ RF_RX, "RX" },         { RF_TRANSITION, "TRANSITION" }, { RF_RESET, "RESET" }
};
#define num_of_trx_states (sizeof(trx_states) / sizeof(trx_states[0])

static struct at86rf215_priv {
	struct spi_device *spi;
	struct spi_message msg;
	struct spi_transfer xfer;
	u8 rx_buf[3];
	u8 tx_buf[3];
} at86rf215_priv;

static struct at86rf215_priv *priv = &at86rf215_priv;

enum { CMD_NOP,
       CMD_SLEEP,
       CMD_TRXOFF,
       CMD_TXPREP,
       CMD_TX,
       CMD_RX,
       CMD_RESET = 0x7 };

void at86rf215_reg_read_complete(void *context)
{
}

void at86rf215_reg_read(u16 addr, u8 val)
{
	int ret;

	ret = spi_async(priv->spi, &priv->msg);
	if (ret < 0) {
		dev_err(&priv->spi->dev, "register read error\n");
	}
}

static struct timer_list ping_timer;

/* RSTN pin number */
static int rstn_pin_num;

static int at86rf215_cookie = 0xbabef00d;

static struct regmap *regmap;

/* Registers whose value can be written */
static bool at86rf215_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case RG_RF09_IRQM:
	case RG_RF09_CMD:
	case RG_RF09_CS:
	case RG_RF09_CCF0L:
	case RG_RF09_CCF0H:
	case RG_RF09_CNL:
	case RG_RF09_CNM:
	case RG_RF09_RXBWC:
	case RG_RF09_RXDFE:
	case RG_RF09_EDD:
	case RG_RF09_RNDV:
	case RG_RF09_TXCUTC:
	case RG_RF09_TXDFE:
	case RG_RF09_PAC:
	case RG_BBC0_IRQM:
	case RG_BBC0_PC:
	case RG_BBC0_OFDMPHRTX:
		return true;

	default:
		return false;
	}
}

/* Registers whose value can be read */
static bool at86rf215_readable_reg(struct device *dev, unsigned int reg)
{
	bool ret;

	ret = at86rf215_writeable_reg(dev, reg);
	if (ret) {
		return ret;
	}

	switch (reg) {
	case RG_RF09_IRQS:
	case RG_RF24_IRQS:
	case RG_RF_PN:
	case RG_RF_VN:
	case RG_RF09_STATE:
	case RG_RF09_RSSI:
		return true;

	default:
		return false;
	}
}

/* Registers whose value shouldn't be cached */
static bool at86rf215_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case RG_RF09_IRQS:
	case RG_RF24_IRQS:
	case RG_RF_PN:  /* TODO: remove */
	case RG_RF_VN:  /* TODO: remove */
	case RG_RF09_STATE:
	case RG_RF09_RSSI:
		return true;

	default:
		return false;
	}
}

/* Registers whose value should not be read outside a call from the driver */
/* The IRQ status registers are cleared automatically by reading the
 * corresponding register via SPI. */
static bool at86rf215_precious_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	/* case RG_RF09_IRQS: */
	case RG_BBC0_IRQS:
		return true;

	default:
		return false;
	}
}

/* AT86RF215 register map configuration */
static const struct regmap_config at86rf215_regmap_config = {
	.reg_bits        = 16,
	.val_bits        = 8,
	.writeable_reg   = at86rf215_writeable_reg,
	.readable_reg    = at86rf215_readable_reg,
	.volatile_reg    = at86rf215_volatile_reg,
	.precious_reg    = at86rf215_precious_reg,
	.read_flag_mask  = 0x0,         /* [0|0|x|x|x|x|x|x|x|x|x|x|x|x|x|x] */
	.write_flag_mask = 0x80,        /* [1|0|x|x|x|x|x|x|x|x|x|x|x|x|x|x] */
	.cache_type      = REGCACHE_RBTREE,
	.max_register    = 0x3FFE,
};

/* Retrieve RSTN pin number from device tree */
static int at86rf215_get_platform_data(struct spi_device *spi,
				       int *rstn_pin_num)
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
	disable_irq_nosync(irq);

	printk("%s: 0x%x\n", __func__, *(int *)data);
	return IRQ_HANDLED;
}

static void at86rf215_configure(void)
{
	int ret;

        /* Radio IRQ mask configuration */
	ret = regmap_write(regmap, RG_RF09_IRQM, 0x1F);
	if (ret < 0) {
		pr_err("can't write radio IRQ mask\n");
		return;
	}

        /* Baseband IRQ mask configuration */
	ret = regmap_write(regmap, RG_BBC0_IRQM, 0x1F);
	if (ret < 0) {
		pr_err("can't write baseband IRQ mask\n");
		return;
	}

        /* Channel configuration */
	ret = regmap_write(regmap, RG_RF09_CS, 0x30);
	if (ret < 0) {
		pr_err("can't write channel spacing\n");
		return;
	}

	ret = regmap_write(regmap, RG_RF09_CCF0L, 0x20);
	if (ret < 0) {
		pr_err("can't write channel center freq0 low byte\n");
		return;
	}

	ret = regmap_write(regmap, RG_RF09_CCF0H, 0x8D);
	if (ret < 0) {
		pr_err("can't write channel center freq0 high byte\n");
		return;
	}

	ret = regmap_write(regmap, RG_RF09_CNL, 0x3);
	if (ret < 0) {
		pr_err("can't write channel number\n");
		return;
	}

	ret = regmap_write(regmap, RG_RF09_CNM, 0);
	if (ret < 0) {
		pr_err("can't trigger channel setting update\n");
		return;
	}

        /*
         * ret = regmap_write(regmap, , );
         * if (ret < 0) {
         *      pr_err("can't write transceiver's \n");
         *      return;
         * }
         */

        /* Frontend configuration */

        /* TXCUTC.LPFCUT is set to 0xB */
	ret = regmap_write(regmap, RG_RF09_TXCUTC, 0xB);
	if (ret < 0) {
		pr_err("can't write TX filter cut-off freq\n");
		return;
	}

        /* TXDFE.SR is set to 3, TXDFE.RCUT is set to 4 */
	ret = regmap_write(regmap, RG_RF09_TXDFE, 0x83);
	if (ret < 0) {
		pr_err("can't write TX sample rate\n");
		return;
	}

        /* PAC.TXPWR is set to 0x1C */
	ret = regmap_write(regmap, RG_RF09_PAC, 0x7C);
	if (ret < 0) {
		pr_err("can't write TX output power\n");
		return;
	}

        /* Energy Measurement and AGC Configuration */
        /* The reset values for the AGC configuration are already suitable for
         * ODFM Option 1 */
	ret = regmap_write(regmap, RG_RF09_EDD, 0x7A);
	if (ret < 0) {
		pr_err("can't write RX energy detection averaging duration\n");
		return;
	}

        /* Modulation configuration */

        /* PC.PT is set to 2 (MR-OFDM) */
	ret = regmap_write(regmap, RG_BBC0_PC, 0x56);
	if (ret < 0) {
		pr_err("can't write PHY type\n");
		return;
	}

        /* OFDMPHRTX.MCS is set to 3 (800kb/s) */
	ret = regmap_write(regmap, RG_BBC0_OFDMPHRTX, 0x3);
	if (ret < 0) {
		pr_err("can't write bit rate\n");
		return;
	}

	/* Initiate transceiver state change */
	ret = regmap_write(regmap, RG_RF09_CMD, RF_TXPREP);
	if (ret < 0) {
		pr_err("can't write command\n");
		return;
	}

	is_configured = true;
}

static void ping_process(struct timer_list *t)
{
	int ret, i;
	unsigned int val;
	const char *description = NULL;

	(void)ret;
	(void)i;
	(void)val;
	(void)description;
	(void)trx_state_descs;

	if (!is_configured) {
		pr_debug("configuring transceiver...");
		at86rf215_configure();
	} else {
		/* pr_debug("transceiver is already configured"); */
	}

#if 0
	ret = regmap_read(regmap, RG_RF_PN, &val);
	if (ret < 0) {
		pr_err("can't read transceiver part number\n");
		return;
	}

	for (i = 0; i < num_of_trx_ids; i++) {
		if (trx_ids[i].part_number == val) {
			description = trx_ids[i].description;
			break;
		}
	}

	if (description == NULL) {
		pr_err("unknown transceiver part number 0x%x\n", val);
		return;
	}

	ret = regmap_read(regmap, RG_RF_VN, &val);
	if (ret < 0) {
		pr_err("can't read transceiver version number\n");
		return;
	}
	pr_debug("detected %sv%d transceiver\n", description, val);

	ret = regmap_read(regmap, RG_RF09_STATE, &val);
	if (ret < 0) {
		pr_err("can't read transceiver's current state\n");
		return;
	}
	pr_debug("transceiver's current state is %s\n", trx_state_descs[val]);

        /* Initiate transceiver state change */
	ret = regmap_write(regmap, RG_RF09_CMD, RF_TXPREP);
	if (ret < 0) {
		pr_err("can't write command\n");
		return;
	}
#endif

	mod_timer(&ping_timer, jiffies + msecs_to_jiffies(1000));
}

#if 0
/* Register read data structures */

static u8 rx_buf[3];
static u8 tx_buf[3];
static struct spi_transfer reg_read_xfer = {
	.len = 3,
	.tx_buf = &tx_buf[0],
	.rx_buf = &rx_buf[0]
};

static void reg_read_complete(void *context)
{
	printk("peekaboo\n");
}

static struct spi_message reg_read_message;

static void reg_read_message_init(void)
{
	spi_message_init(&reg_read_message);
	reg_read_message.complete = reg_read_complete;
	spi_message_add_tail(&reg_read_xfer, &reg_read_message);
}

static int __at86rf215_reg_read(struct spi_device *spi, u16 reg_addr)
{
	int ret = -1;

	*((u16 *)tx_buf) = htons(reg_addr) |;
	ret = spi_async(spi, &reg_read_message);

	if (ret < 0) {
		dev_err(&spi->dev, "register read error\n");
		return ret;
	}

	return 0;
}
#endif

/*
 * static int at86rf215_regmap_read(struct spi_device *spi, unsigned int addr,
 *                               unsigned int *val)
 * {
 *      return regmap_read(regmap, addr, val);
 * }
 */

static int at86rf215_probe(struct spi_device *spi)
{
	unsigned int val;
        unsigned long irqflags = 0;

	(void)val;

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
	if (devm_gpio_request_one(&spi->dev, rstn_pin_num, GPIOF_OUT_INIT_HIGH,
				  "rstn") < 0) {
		return -1;
	}

        /* TODO: reset transceiver by asserting RTSN low */

        /* TODO: initialize register map */
	regmap = devm_regmap_init_spi(spi, &at86rf215_regmap_config);
	if (IS_ERR(regmap)) {
		int ret = PTR_ERR(regmap);
		dev_err(&spi->dev, "can't alloc regmap 0x%x\n", ret);
		devm_gpio_free(&spi->dev, rstn_pin_num);
		return -1;
	}

	priv->spi = spi;
	spi_message_init(&priv->msg);
	priv->msg.complete = at86rf215_reg_read_complete;
	spi_message_add_tail(&priv->xfer, &priv->msg);

	timer_setup(&ping_timer, ping_process, 0);

	/* Read IRQ status regs to reset line */
	regmap_read(regmap, RG_RF09_IRQS, &val);
	regmap_read(regmap, RG_RF24_IRQS, &val);

	ping_process(NULL);

        /* reg_read_message_init(); */

        /* TODO: datasheet section 7.2.1 common configuration */

	irqflags = irq_get_trigger_type(spi->irq);
	dev_dbg(&spi->dev, "irqflags 0x%lx\n", irqflags);
	if (!irqflags) {
		irqflags = IRQF_TRIGGER_HIGH;
	}

	if (devm_request_irq(&spi->dev, spi->irq, at86rf215_irq_handler,
			     IRQF_SHARED | irqflags,
			     dev_name(&spi->dev), &at86rf215_cookie) < 0) {
		dev_err(&spi->dev, "can't allocate IRQ\n");
		devm_gpio_free(&spi->dev, rstn_pin_num);
		return -1;
	}

	/* disable_irq(spi->irq); */

	return 0;
}

static int at86rf215_remove(struct spi_device *spi)
{
	if (del_timer(&ping_timer)) {
		pr_debug("deactivated active timer\n");
	}

	devm_free_irq(&spi->dev, spi->irq, &at86rf215_cookie);
	devm_gpio_free(&spi->dev, rstn_pin_num);

	return 0;
}

static const struct of_device_id at86rf215_of_match[] = {
	{
		.compatible = "atmel,at86rf233",
	},
	{},
};
MODULE_DEVICE_TABLE(of, at86rf215_of_match);

static const struct spi_device_id at86rf215_device_id[] = {
	{
		.name = "at86rf233",
	},
	{},
};
MODULE_DEVICE_TABLE(spi, at86rf215_device_id);

static struct spi_driver at86rf215_driver = {
	.id_table = at86rf215_device_id,
	.driver   = { .of_match_table = of_match_ptr(at86rf215_of_match),
		      .name           = "at86rf233" },
	.probe    = at86rf215_probe,
	.remove   = at86rf215_remove
};

module_spi_driver(at86rf215_driver);

MODULE_DESCRIPTION("Atmel AT86RF215 radio transceiver driver");
MODULE_LICENSE("GPL v2");
