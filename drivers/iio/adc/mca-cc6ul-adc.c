/* mca-cc6ul-adc.c - ADC driver for MCA on ConnectCore 6UL
 *
 * Copyright (C) 2016  Digi International Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <linux/iio/iio.h>
#include <linux/mfd/mca-cc6ul/core.h>
#include <linux/mfd/mca-cc6ul/registers.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct mca_cc6ul_adc {
	struct mca_cc6ul *mca;
	u8 num_adcs;
};

static int mca_adc_read_raw(struct iio_dev *iio,
			    struct iio_chan_spec const *channel, int *value,
			    int *shift, long mask)
{
	struct mca_cc6ul_adc *adc = iio_priv(iio);
	u16 val;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_bulk_read(adc->mca->regmap,
				       MCA_REG_ADC_VAL_L_0 + channel->channel * 2,
				       &val, sizeof(val));
		if (ret < 0) {
			dev_err(adc->mca->dev, "Error reading ADC%d value (%d)\n",
				channel->channel, ret);
			return ret;
		}

		dev_dbg(adc->mca->dev, "ADC%d = 0x%04x\n", channel->channel, val);
		*value = val;
		return IIO_VAL_INT;

	default:
		break;
	}

	return -EINVAL;
}

static const struct iio_info mca_adc_info = {
	.read_raw = mca_adc_read_raw,
	.driver_module = THIS_MODULE,
};

static int mca_cc6ul_adc_probe(struct platform_device *pdev)
{
	struct mca_cc6ul *mca = dev_get_drvdata(pdev->dev.parent);
	struct mca_cc6ul_adc *mca_adc;
	struct iio_dev *iio;
	struct device_node *np;
	struct iio_chan_spec *chan;
	u8 adc_ch_list[MCA_CC6UL_MAX_IOS];
	struct property *prop;
	const __be32 *cur;
	u32 ch, cfg;
	int ret = 0;

	if (!mca || !mca->dev || !mca->dev->parent ||
	    !mca->dev->parent->of_node)
		return -EPROBE_DEFER;

	iio = devm_iio_device_alloc(&pdev->dev, sizeof(*mca_adc));
	if (!iio) {
		dev_err(&pdev->dev, "Failed to allocate iio device\n");
		return -ENOMEM;
	}
	mca_adc = iio_priv(iio);
	mca_adc->mca = mca;
	mca_adc->num_adcs = 0;

	/* Find entry in device-tree */
	if (mca->dev->of_node) {
		/* Return if mca_adc node does not exist or if it is disabled */
		np = of_find_compatible_node(mca->dev->of_node, NULL,
					     "digi,mca-cc6ul-adc");
		if (!np || !of_device_is_available(np))
			return -ENODEV;

		of_property_for_each_u32(np, "digi,adc-ch-list",
					 prop, cur, ch) {
			if (ch >= MCA_CC6UL_MAX_IOS)
				continue;

			/*
			 * Verify that the requested IOs are ADC capable and
			 * enable the channel for ADC operation
			 */
			ret = regmap_read(mca->regmap,
				  MCA_REG_ADC_CFG_0 + ch, &cfg);
			if (ret != 0) {
				dev_err(mca->dev,
					"Failed read ADC%d CFG register (%d)\n",
					ch, ret);
				goto error_dev_free;
			}

			/* Remove the channel from the list if not capable */
			if (!(cfg & MCA_REG_ADC_CAPABLE)) {
				dev_warn(mca->dev,
					 "Removing ADC%d, IO no ADC capable\n",
					 ch);
				continue;
			}

			/* Enable the channel for ADC operation*/
			ret = regmap_update_bits(mca->regmap,
						 MCA_REG_ADC_CFG_0 + ch,
						 MCA_REG_ADC_EN, MCA_REG_ADC_EN);
			if (ret != 0) {
				dev_err(mca->dev, "Error enabling ADC%d (%d)\n",
					ch, ret);
				goto error_dev_free;
			}
			adc_ch_list[mca_adc->num_adcs] = (u8)ch;
			mca_adc->num_adcs++;
		}
	}

	if (mca_adc->num_adcs == 0)
		goto error_dev_free;

	iio->dev.parent = &pdev->dev;
	iio->name = dev_name(&pdev->dev);
	iio->modes = INDIO_DIRECT_MODE;
	iio->info = &mca_adc_info;
	iio->channels = devm_kzalloc(&pdev->dev,
				     mca_adc->num_adcs * sizeof(struct iio_chan_spec),
				     GFP_KERNEL);
	if (!iio->channels)
		goto error_dev_free;

	/* Initialize the ADC channels */
	for (iio->num_channels = 0, chan = (struct iio_chan_spec *)iio->channels;
	     iio->num_channels < mca_adc->num_adcs;
	     iio->num_channels++, chan++) {
		chan->type = IIO_VOLTAGE;
		chan->indexed = 1;
		chan->channel = adc_ch_list[iio->num_channels];
		chan->scan_index = iio->num_channels;
		chan->scan_type.sign = 'u';
		chan->scan_type.realbits = 12;
		chan->scan_type.storagebits = 16;
		chan->info_mask_separate = BIT(IIO_CHAN_INFO_RAW);
	}

	ret = iio_device_register(iio);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register mca adc iio device\n");
		goto error_free_ch;
	}

	platform_set_drvdata(pdev, iio);

	pr_info("ADC driver for MCA of CC6UL\n");

	return 0;

error_free_ch:
	devm_kfree(&pdev->dev, (void *)iio->channels);

error_dev_free:
	devm_iio_device_free(&pdev->dev, iio);

	return ret;
}

static int mca_cc6ul_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *iio = platform_get_drvdata(pdev);

	devm_kfree(&pdev->dev, (void *)iio->channels);
	iio_device_unregister(iio);
	devm_iio_device_free(&pdev->dev, iio);

	return 0;
}

static const struct of_device_id mca_cc6ul_adc_ids[] = {
        { .compatible = "digi,mca-cc6ul-adc", },
        { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mca_cc6ul__ids);

static struct platform_driver mca_cc6ul_adc_driver = {
	.probe	= mca_cc6ul_adc_probe,
	.remove	= mca_cc6ul_adc_remove,
	.driver	= {
		.name	= MCA_CC6UL_DRVNAME_ADC,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(mca_cc6ul_adc_ids),
	},
};

static int __init mca_cc6ul_adc_init(void)
{
	return platform_driver_register(&mca_cc6ul_adc_driver);
}
module_init(mca_cc6ul_adc_init);

static void __exit mca_cc6ul_adc_exit(void)
{
	platform_driver_unregister(&mca_cc6ul_adc_driver);
}
module_exit(mca_cc6ul_adc_exit);


MODULE_AUTHOR("Digi International Inc");
MODULE_DESCRIPTION("ADC driver for MCA of ConnectCore 6UL");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" MCA_CC6UL_DRVNAME_ADC);
