#include <linux/module.h>
#include <linux/platform_device.h>
#include <net/mac802154.h>

struct at86rf215_private {
	struct ieee802154_hw *hw;
};

static struct at86rf215_private *priv;

static int at86rf215_start(struct ieee802154_hw *hw)
{
	return 0;
}

static void at86rf215_stop(struct ieee802154_hw *hw)
{
}

static int at86rf215_xmit_async(struct ieee802154_hw *hw, struct sk_buff *skb)
{
	return 0;
}

static int at86rf215_ed(struct ieee802154_hw *hw, u8 *level)
{
	return 0;
}

static int at86rf215_set_channel(struct ieee802154_hw *hw, u8 page, u8 channel)
{
	return 0;
}

static const struct ieee802154_ops at86rf215_ieee802154_ops = {
	.start = at86rf215_start,
	.stop = at86rf215_stop,
	.xmit_async = at86rf215_xmit_async,
	.ed = at86rf215_ed,
	.set_channel = at86rf215_set_channel
};

static int at86rf215_probe(struct platform_device *pdev)
{
	struct ieee802154_hw *hw;
	int ret = 0;

	if (priv != NULL)
	{
		return -1;
	}

	hw = ieee802154_alloc_hw(sizeof(priv), &at86rf215_ieee802154_ops);
	if (!hw)
		return -ENOMEM;

	priv = hw->priv;
	priv->hw = hw;

	ieee802154_random_extended_addr(&hw->phy->perm_extended_addr);

	ret = ieee802154_register_hw(hw);
	if (ret)
		ieee802154_free_hw(hw);

	return ret;
}

static int at86rf215_remove(struct platform_device *pdev)
{
	ieee802154_unregister_hw(priv->hw);
	ieee802154_free_hw(priv->hw);

	priv = NULL;

	return 0;
}

static struct platform_driver at86rf215_driver = {
	.driver = {
		.name = "at86rf215",
	},
	.probe = at86rf215_probe,
	.remove = at86rf215_remove
};

static struct platform_device *at86rf215_device;

static __init int at86rf215_init(void)
{
	int ret;

	ret = platform_driver_register(&at86rf215_driver);
	if (ret < 0) {
		printk(KERN_ERR "can't register AT86RF215 driver: %d\n", ret);
		return ret;
	}

	at86rf215_device = platform_device_register_simple("at86rf215", -1,
							   NULL, 0);
	if (IS_ERR(at86rf215_device)) {
		printk(KERN_ERR "can't register AT86RF215 device\n");
		platform_driver_unregister(&at86rf215_driver);
		return PTR_ERR(at86rf215_device);
	}

	return 0;
}

static __exit void at86rf215_exit(void)
{
	platform_device_unregister(at86rf215_device);
	platform_driver_unregister(&at86rf215_driver);
}

module_init(at86rf215_init);
module_exit(at86rf215_exit);

MODULE_LICENSE("GPL v2");
