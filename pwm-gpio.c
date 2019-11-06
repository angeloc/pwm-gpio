// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019 Angelo Compagnucci <angelo.compagnucci@gmail.com>
 *
 * pwm-gpio.c - driver for pwm output on gpio via high resolution timers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

struct gpio_pwm_chip {
	struct pwm_chip chip;
};

struct gpio_pwm_data {

	bool is_running;
	bool polarity;
	bool pin_on;
	unsigned int on_time;
	unsigned int off_time;
	struct mutex lock;
	struct hrtimer timer;
	struct gpio_desc *gpio_pwm;
};

static void gpio_pwm_off(struct gpio_pwm_data *gpio_data)
{
	gpiod_set_value_cansleep(gpio_data->gpio_pwm,
		gpio_data->polarity ? 1 : 0);
	gpio_data->pin_on = false;
}

static void gpio_pwm_on(struct gpio_pwm_data *gpio_data)
{
	gpiod_set_value_cansleep(gpio_data->gpio_pwm,
		gpio_data->polarity ? 0 : 1);
	gpio_data->pin_on = true;
}

enum hrtimer_restart gpio_pwm_timer(struct hrtimer *timer)
{
	struct gpio_pwm_data *gpio_data = container_of(timer,
						  struct gpio_pwm_data,
						  timer);

	if (!gpio_data->pin_on) {
		gpio_pwm_on(gpio_data);
		hrtimer_forward_now(&gpio_data->timer,
				    ns_to_ktime(gpio_data->on_time));
	} else {
		gpio_pwm_off(gpio_data);
		hrtimer_forward_now(&gpio_data->timer,
				    ns_to_ktime(gpio_data->off_time));
	}

	return HRTIMER_RESTART;
}

static int gpio_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			    int duty_ns, int period_ns)
{
	struct gpio_pwm_data *gpio_data = pwm_get_chip_data(pwm);

	mutex_lock(&gpio_data->lock);
	gpio_data->on_time = duty_ns;
	gpio_data->off_time = period_ns - duty_ns;
	mutex_unlock(&gpio_data->lock);

	return 0;
}

static int gpio_pwm_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				 enum pwm_polarity polarity)
{
	struct gpio_pwm_data *gpio_data = pwm_get_chip_data(pwm);

	mutex_lock(&gpio_data->lock);
	gpio_data->polarity = (polarity != PWM_POLARITY_NORMAL) ? true : false;
	mutex_unlock(&gpio_data->lock);

	return 0;
}

static int gpio_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct gpio_pwm_data *gpio_data = pwm_get_chip_data(pwm);

	mutex_lock(&gpio_data->lock);

	if (gpio_data->is_running) {
		mutex_unlock(&gpio_data->lock);
		return -EBUSY;
	}

	gpio_data->is_running = true;
	hrtimer_start(&gpio_data->timer, ktime_set(0, 0),
			HRTIMER_MODE_REL);

	mutex_unlock(&gpio_data->lock);

	return 0;
}

static void gpio_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct gpio_pwm_data *gpio_data = pwm_get_chip_data(pwm);

	mutex_lock(&gpio_data->lock);

	if (!gpio_data->is_running) {
		mutex_unlock(&gpio_data->lock);
		return;
	}

	hrtimer_cancel(&gpio_data->timer);
	gpio_pwm_off(gpio_data);
	gpio_data->is_running = false;

	mutex_unlock(&gpio_data->lock);
}

static int gpio_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct gpio_pwm_data *gpio_data;
	struct gpio_desc *gpio_pwm;

	gpio_data = kzalloc(sizeof(*gpio_data), GFP_KERNEL);
	if (!gpio_data)
		return -ENOMEM;

	gpio_pwm = gpiod_get(chip->dev, "pwm", GPIOD_OUT_LOW);
	if (IS_ERR(gpio_pwm)) {
		dev_err(chip->dev, "failed to retrieve pwm from dts\n");
		return PTR_ERR(gpio_pwm);
	}

	hrtimer_init(&gpio_data->timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	if (!hrtimer_is_hres_active(&gpio_data->timer))
		dev_warn(chip->dev, "HR timer unavailable, restricting to \
				     low resolution\n");

	gpio_data->timer.function = &gpio_pwm_timer;
	gpio_data->gpio_pwm = gpio_pwm;
	gpio_data->pin_on = false;
	gpio_data->is_running = false;

	return pwm_set_chip_data(pwm, gpio_data);
}

static void gpio_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct gpio_pwm_data *gpio_data = pwm_get_chip_data(pwm);

	gpio_pwm_disable(chip, pwm);
	gpiod_put(gpio_data->gpio_pwm);
	kfree(gpio_data);
}

static const struct pwm_ops gpio_pwm_ops = {
	.config         = gpio_pwm_config,
	.set_polarity   = gpio_pwm_set_polarity,
	.enable         = gpio_pwm_enable,
	.disable        = gpio_pwm_disable,
	.request        = gpio_pwm_request,
	.free           = gpio_pwm_free,
	.owner          = THIS_MODULE,
};

static int gpio_pwm_probe(struct platform_device *pdev)
{
	struct gpio_pwm_chip *gpio_chip;
	int ret;

	gpio_chip = devm_kzalloc(&pdev->dev, sizeof(*gpio_chip), GFP_KERNEL);
	if (!gpio_chip)
		return -ENOMEM;

	gpio_chip->chip.dev = &pdev->dev;
	gpio_chip->chip.ops = &gpio_pwm_ops;
	gpio_chip->chip.base = -1;
	gpio_chip->chip.npwm = 1;

	ret = pwmchip_add(&gpio_chip->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add pwm gpio chip %d\n", ret);
		return -ENODEV;
	}

	platform_set_drvdata(pdev, gpio_chip);

	return 0;
}

static int gpio_pwm_remove(struct platform_device *pdev)
{
	struct gpio_pwm_chip *gpio_chip = platform_get_drvdata(pdev);

	return pwmchip_remove(&gpio_chip->chip);
}

static const struct of_device_id gpio_pwm_of_match[] = {
	{ .compatible = "pwm-gpio", },
	{},
};
MODULE_DEVICE_TABLE(of, gpio_pwm_of_match);

static struct platform_driver gpio_pwm_driver = {
	.probe = gpio_pwm_probe,
	.remove = gpio_pwm_remove,
	.driver = {
		.name = "pwm-gpio",
		.of_match_table = of_match_ptr(gpio_pwm_of_match),
	},
};
module_platform_driver(gpio_pwm_driver);

MODULE_AUTHOR("Angelo Compagnucci <angelo.compagnucci@gmail.com>");
MODULE_DESCRIPTION("Generic GPIO bit-banged PWM driver");
MODULE_LICENSE("GPL");
