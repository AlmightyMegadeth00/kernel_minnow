/*
 * Copyright (C) 2007 - 2010 Motorola, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/gpio.h>

#include <linux/regulator/consumer.h>

#include <linux/spi/cpcap.h>
#include <linux/spi/cpcap-regbits.h>
#include <linux/spi/spi.h>

#define MS_TO_NS(x)		((x) * NSEC_PER_MSEC)
#define CPCAP_SENSE4_LS		8
#define CPCAP_BIT_DP_S_LS	(CPCAP_BIT_DP_S << CPCAP_SENSE4_LS)
#define CPCAP_BIT_DM_S_LS	(CPCAP_BIT_DM_S << CPCAP_SENSE4_LS)

#define SENSE_USB           (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_CHRGCURR1_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

#define SENSE_2WIRE         (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_CHRGCURR1_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S   | \
			     CPCAP_BIT_DP_S_LS)

#define SENSE_USB_FLASH     (CPCAP_BIT_CHRGCURR1_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

#define SENSE_FACTORY       (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_ID_GROUND_S | \
			     CPCAP_BIT_CHRGCURR1_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

/* This Sense mask is needed because on TI the CHRGCURR1 interrupt is not always
 * set.  In Factory Mode the comparator follows the Charge current only. */
#define SENSE_FACTORY_COM   (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_ID_GROUND_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

#define SENSE_CHARGER_FLOAT (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_CHRGCURR1_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S   | \
			     CPCAP_BIT_SE1_S       | \
			     CPCAP_BIT_DM_S_LS     | \
			     CPCAP_BIT_DP_S_LS)

#define SENSE_CHARGER       (CPCAP_BIT_CHRGCURR1_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S   | \
				 CPCAP_BIT_SE1_S       | \
				 CPCAP_BIT_DM_S_LS     | \
				 CPCAP_BIT_DP_S_LS)

#define SENSE_IDLOW_CHARGER  (CPCAP_BIT_CHRGCURR1_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S   | \
			     CPCAP_BIT_ID_GROUND_S | \
			     CPCAP_BIT_DP_S_LS)

#define SENSE_CHARGER_MASK  (CPCAP_BIT_ID_GROUND_S | \
			     CPCAP_BIT_SESSVLD_S)

#ifdef CONFIG_CHARGER_CPCAP_2WIRE
#define TWOWIRE_HANDSHAKE_LEN   6   /* Number of bytes in handshake sequence */
#define TWOWIRE_DELAY           MS_TO_NS(10)  /* delay between edges in ns */
#define BI2BY                   8   /* bits per byte */
#define TWOWIRE_HANDSHAKE_SEQUENCE {0x07, 0xC1, 0xF3, 0xE7, 0xCF, 0x9F}
#endif

#define UNDETECT_TRIES		5

#define CPCAP_USB_DET_PRINT_STATUS (1U << 0)
#define CPCAP_USB_DET_PRINT_TRANSITION (1U << 1)
static int cpcap_usb_det_debug_mask;

module_param_named(cpcap_usb_det_debug_mask, cpcap_usb_det_debug_mask, int,
		   S_IRUGO | S_IWUSR | S_IWGRP);

#define cpcap_usb_det_debug(debug_level_mask, args...) \
	do { \
		if (cpcap_usb_det_debug_mask & \
		    CPCAP_USB_DET_PRINT_##debug_level_mask) { \
			pr_info(args); \
		} \
	} while (0)

enum cpcap_det_state {
	CONFIG,
	SAMPLE_1,
	SAMPLE_2,
	IDENTIFY,
	USB,
	FACTORY,
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
	START2WIRE,
	FINISH2WIRE,
#endif
};

enum cpcap_accy {
	CPCAP_ACCY_USB,
	CPCAP_ACCY_FACTORY,
	CPCAP_ACCY_CHARGER,
	CPCAP_ACCY_NONE,
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
	CPCAP_ACCY_2WIRE,
#endif
	/* Used while debouncing the accessory. */
	CPCAP_ACCY_UNKNOWN,
};

#ifdef CONFIG_CHARGER_CPCAP_2WIRE
enum cpcap_twowire_state {
	CPCAP_TWOWIRE_RUNNING,
	CPCAP_TWOWIRE_DONE,
};

struct cpcap_usb_det_2wire {
	int gpio;
	unsigned short pos;
	unsigned char data[TWOWIRE_HANDSHAKE_LEN];
	enum cpcap_twowire_state state;
};
#endif

struct cpcap_usb_det_data {
	struct cpcap_device *cpcap;
	struct delayed_work work;
	unsigned short sense;
	unsigned short prev_sense;
	enum cpcap_det_state state;
	enum cpcap_accy usb_accy;
	struct platform_device *usb_dev;
	struct platform_device *usb_connected_dev;
	struct platform_device *charger_connected_dev;
	struct regulator *regulator;
	struct wake_lock wake_lock;
	unsigned char is_vusb_enabled;
	unsigned char undetect_cnt;
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
	struct hrtimer hr_timer;
	struct cpcap_usb_det_2wire twowire_data;
#endif
};

static unsigned char vbus_valid_adc_check(struct cpcap_usb_det_data *data);

static const char *accy_devices[] = {
	"cpcap_usb_charger",
	"cpcap_factory",
	"cpcap_charger",
};

#ifdef CONFIG_USB_TESTING_POWER
static int testing_power_enable = -1;
module_param(testing_power_enable, int, 0644);
MODULE_PARM_DESC(testing_power_enable, "Enable factory cable power "
	"supply function for testing");
#endif

static void vusb_enable(struct cpcap_usb_det_data *data)
{
	int ret;
	if (!data->is_vusb_enabled) {
		wake_lock(&data->wake_lock);
		ret = regulator_enable(data->regulator);
		data->is_vusb_enabled = 1;
	}
}

static void vusb_disable(struct cpcap_usb_det_data *data)
{
	int ret;
	if (data->is_vusb_enabled) {
		wake_unlock(&data->wake_lock);
		ret = regulator_disable(data->regulator);
		data->is_vusb_enabled = 0;
	}
}

static int get_sense(struct cpcap_usb_det_data *data)
{
	int retval = -EFAULT;
	unsigned short value;
	struct cpcap_device *cpcap;

	if (!data)
		return -EFAULT;
	cpcap = data->cpcap;

	retval = cpcap_regacc_read(cpcap, CPCAP_REG_INTS1, &value);
	if (retval)
		return retval;

	/* Clear ASAP after read. */
	retval = cpcap_regacc_write(cpcap, CPCAP_REG_INT1,
				     (CPCAP_BIT_CHRG_DET_I |
				      CPCAP_BIT_ID_FLOAT_I |
				      CPCAP_BIT_ID_GROUND_I),
				     (CPCAP_BIT_CHRG_DET_I |
				      CPCAP_BIT_ID_FLOAT_I |
				      CPCAP_BIT_ID_GROUND_I));
	if (retval)
		return retval;

	data->sense = value & (CPCAP_BIT_ID_FLOAT_S |
			       CPCAP_BIT_ID_GROUND_S);

	retval = cpcap_regacc_read(cpcap, CPCAP_REG_INTS2, &value);
	if (retval)
		return retval;

	/* Clear ASAP after read. */
	retval = cpcap_regacc_write(cpcap, CPCAP_REG_INT2,
				    (CPCAP_BIT_CHRGCURR1_I |
				     CPCAP_BIT_VBUSVLD_I |
				     CPCAP_BIT_SESSVLD_I |
				     CPCAP_BIT_SE1_I),
				    (CPCAP_BIT_CHRGCURR1_I |
				     CPCAP_BIT_VBUSVLD_I |
				     CPCAP_BIT_SESSVLD_I |
				     CPCAP_BIT_SE1_I));
	if (retval)
		return retval;

	data->sense |= value & (CPCAP_BIT_CHRGCURR1_S |
				CPCAP_BIT_VBUSVLD_S |
				CPCAP_BIT_SESSVLD_S |
				CPCAP_BIT_SE1_S);

	retval = cpcap_regacc_read(cpcap, CPCAP_REG_INTS4, &value);
	if (retval)
		return retval;

	/* Clear ASAP after read. */
	retval = cpcap_regacc_write(cpcap, CPCAP_REG_INT4,
				     (CPCAP_BIT_DP_I |
				      CPCAP_BIT_DM_I),
				     (CPCAP_BIT_DP_I |
				      CPCAP_BIT_DM_I));
	if (retval)
		return retval;

	data->sense |= (value & (CPCAP_BIT_DP_S |
			       CPCAP_BIT_DM_S)) << CPCAP_SENSE4_LS;

	return 0;
}

static int configure_hardware(struct cpcap_usb_det_data *data,
			      enum cpcap_accy accy)
{
	int retval;

	/* Take control of pull up from ULPI. */
	retval  = cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC3,
				     CPCAP_BIT_PU_SPI,
				     CPCAP_BIT_PU_SPI);
	retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1,
				    CPCAP_BIT_DP150KPU,
				    (CPCAP_BIT_DP150KPU | CPCAP_BIT_DP1K5PU |
				     CPCAP_BIT_DM1K5PU | CPCAP_BIT_DPPD |
				     CPCAP_BIT_DMPD));

	switch (accy) {
	case CPCAP_ACCY_USB:
	case CPCAP_ACCY_FACTORY:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1, 0,
					     CPCAP_BIT_VBUSPD);
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC2,
					     CPCAP_BIT_USBXCVREN,
					     CPCAP_BIT_USBXCVREN);
		/* Give USB driver control of pull up via ULPI. */
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC3,
					     0,
					     CPCAP_BIT_PU_SPI |
					     CPCAP_BIT_DMPD_SPI |
					     CPCAP_BIT_DPPD_SPI |
					     CPCAP_BIT_SUSPEND_SPI |
					     CPCAP_BIT_ULPI_SPI_SEL);

		if ((data->cpcap->vendor == CPCAP_VENDOR_ST) &&
			(data->cpcap->revision == CPCAP_REVISION_2_0))
				vusb_enable(data);

		break;

	case CPCAP_ACCY_CHARGER:
		/* Disable Reverse Mode */
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_CRM,
					     0, CPCAP_BIT_RVRSMODE);
		/* Enable VBus PullDown */
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1,
					     CPCAP_BIT_VBUSPD,
					     CPCAP_BIT_VBUSPD);
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC3, 0,
					     CPCAP_BIT_VBUSSTBY_EN);
		break;

	case CPCAP_ACCY_UNKNOWN:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1, 0,
					     CPCAP_BIT_VBUSPD);
		break;

	case CPCAP_ACCY_NONE:
	default:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1,
					     CPCAP_BIT_VBUSPD,
					     CPCAP_BIT_VBUSPD);
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC2, 0,
					     CPCAP_BIT_USBXCVREN);
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC3,
					     CPCAP_BIT_DMPD_SPI |
					     CPCAP_BIT_DPPD_SPI |
					     CPCAP_BIT_SUSPEND_SPI |
					     CPCAP_BIT_ULPI_SPI_SEL,
					     CPCAP_BIT_DMPD_SPI |
					     CPCAP_BIT_DPPD_SPI |
					     CPCAP_BIT_SUSPEND_SPI |
					     CPCAP_BIT_ULPI_SPI_SEL);
		break;
	}

	if (retval != 0)
		retval = -EFAULT;

	return retval;
}

static unsigned char vbus_valid_adc_check(struct cpcap_usb_det_data *data)
{
	struct cpcap_adc_request req;
	int ret;

	req.format = CPCAP_ADC_FORMAT_CONVERTED;
	req.timing = CPCAP_ADC_TIMING_IMM;
	req.type = CPCAP_ADC_TYPE_BANK_0;

	ret = cpcap_adc_sync_read(data->cpcap, &req);
	if (ret) {
		dev_err(&data->cpcap->spi->dev,
		 "%s: ADC Read failed\n", __func__);
		return false;
	}
	return ((req.result[CPCAP_ADC_CHG_ISENSE] < 50) &&
		(req.result[CPCAP_ADC_VBUS] <
		(req.result[CPCAP_ADC_BATTP]))) ? false : true;
}


static void notify_accy(struct cpcap_usb_det_data *data, enum cpcap_accy accy)
{
	dev_info(&data->cpcap->spi->dev, "notify_accy: accy=%d\n", accy);

	if ((data->usb_accy != CPCAP_ACCY_NONE) && (data->usb_dev != NULL)) {
		platform_device_del(data->usb_dev);
		data->usb_dev = NULL;
	}

	configure_hardware(data, accy);
	data->usb_accy = accy;

	if (accy != CPCAP_ACCY_NONE) {
		data->usb_dev = platform_device_alloc(accy_devices[accy], -1);
		if (data->usb_dev) {
			data->usb_dev->dev.platform_data = data->cpcap;
			platform_device_add(data->usb_dev);
		}
	} else
		vusb_disable(data);

	if ((accy == CPCAP_ACCY_USB) || (accy == CPCAP_ACCY_FACTORY)) {
		if (!data->usb_connected_dev) {
			data->usb_connected_dev =
			    platform_device_alloc("cpcap_usb_connected", -1);
			platform_device_add(data->usb_connected_dev);
		}
	} else if (data->usb_connected_dev) {
		platform_device_del(data->usb_connected_dev);
		data->usb_connected_dev = NULL;
	}

	if (accy == CPCAP_ACCY_CHARGER) {
		if (!data->charger_connected_dev) {
			data->charger_connected_dev =
			    platform_device_alloc("cpcap_charger_connected",
						  -1);
			platform_device_add(data->charger_connected_dev);
		}
	} else if (data->charger_connected_dev) {
		platform_device_del(data->charger_connected_dev);
		data->charger_connected_dev = NULL;
	}
}

#ifdef CONFIG_CHARGER_CPCAP_2WIRE
static enum hrtimer_restart cpcap_send_2wire_sendbit(struct hrtimer *timer)
{
	struct cpcap_usb_det_data *usb_det_data =
		container_of(timer, struct cpcap_usb_det_data, hr_timer);
	struct cpcap_usb_det_2wire *twd = &(usb_det_data->twowire_data);
	enum hrtimer_restart ret = HRTIMER_NORESTART;
	bool value;

	if (gpio_is_valid(twd->gpio) &&
	   (twd->pos < TWOWIRE_HANDSHAKE_LEN * BI2BY)) {
		value = !!(twd->data[twd->pos/BI2BY] &
			(1 << (BI2BY - (twd->pos % BI2BY) - 1)));
		gpio_set_value(twd->gpio, value);
		ret = HRTIMER_RESTART;
	}

	if (++twd->pos == TWOWIRE_HANDSHAKE_LEN * BI2BY ||
		!gpio_is_valid(twd->gpio)) {
		twd->state = CPCAP_TWOWIRE_DONE;
		ret = HRTIMER_NORESTART;
	}

	if (ret == HRTIMER_RESTART)
		hrtimer_forward(timer, ktime_get(), ns_to_ktime(TWOWIRE_DELAY));

	return ret;
}
#endif

static void detection_work(struct work_struct *work)
{
	struct cpcap_usb_det_data *data =
		container_of(work, struct cpcap_usb_det_data, work.work);
	unsigned char isVBusValid = 0;
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
	ktime_t next_time;
	int sessvalid;
	unsigned char handshake[TWOWIRE_HANDSHAKE_LEN] =
						 TWOWIRE_HANDSHAKE_SEQUENCE;
#endif

	switch (data->state) {
	case CONFIG:
		vusb_enable(data);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_CHRG_DET);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_CHRG_CURR1);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_SE1);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_IDGND);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_VBUSVLD);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_IDFLOAT);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_DPI);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_DMI);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_SESSVLD);

		configure_hardware(data, CPCAP_ACCY_UNKNOWN);

		data->undetect_cnt = 0;
		data->state = SAMPLE_1;
		schedule_delayed_work(&data->work, msecs_to_jiffies(11));
		break;

	case SAMPLE_1:
		get_sense(data);
		data->state = SAMPLE_2;
		schedule_delayed_work(&data->work, msecs_to_jiffies(100));
		break;

	case SAMPLE_2:
		data->prev_sense = data->sense;
		get_sense(data);

		if (data->prev_sense != data->sense) {
			/* Stay in this state */
			data->state = SAMPLE_2;
			schedule_delayed_work(&data->work,
					      msecs_to_jiffies(100));
		} else if (!(data->sense & CPCAP_BIT_SE1_S) &&
			   (data->sense & CPCAP_BIT_ID_FLOAT_S) &&
			   !(data->sense & CPCAP_BIT_ID_GROUND_S) &&
			   !(data->sense & CPCAP_BIT_SESSVLD_S)) {
			data->state = IDENTIFY;
			schedule_delayed_work(&data->work,
					      msecs_to_jiffies(100));
		} else {
			data->state = IDENTIFY;
			schedule_delayed_work(&data->work, 0);
		}
		break;

	case IDENTIFY:
		get_sense(data);
		data->state = CONFIG;
		isVBusValid = vbus_valid_adc_check(data);

		if ((data->sense == SENSE_USB) ||
		    (data->sense == SENSE_USB_FLASH)) {
			notify_accy(data, CPCAP_ACCY_USB);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_CURR1);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);

			/* Special handling of USB cable undetect. */
			data->state = USB;
		} else if ((data->sense == SENSE_FACTORY) ||
			   (data->sense == SENSE_FACTORY_COM)) {
#ifdef CONFIG_USB_TESTING_POWER
			if (testing_power_enable > 0) {
				notify_accy(data, CPCAP_ACCY_NONE);
				cpcap_irq_unmask(data->cpcap,
					CPCAP_IRQ_CHRG_DET);
				cpcap_irq_unmask(data->cpcap,
					CPCAP_IRQ_CHRG_CURR1);
				cpcap_irq_unmask(data->cpcap,
				CPCAP_IRQ_VBUSVLD);
				break;
			}
#endif
			notify_accy(data, CPCAP_ACCY_FACTORY);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);

			/* Special handling of factory cable undetect. */
			data->state = FACTORY;
		} else if (((data->sense | CPCAP_BIT_VBUSVLD_S) == \
				SENSE_CHARGER_FLOAT) ||
			   ((data->sense | CPCAP_BIT_VBUSVLD_S) == \
				SENSE_CHARGER) ||
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
			   (data->usb_accy == CPCAP_ACCY_2WIRE) ||
#endif
			   (data->sense == SENSE_IDLOW_CHARGER)) {

			if ((isVBusValid) && ((data->sense == \
				SENSE_CHARGER_FLOAT) ||
				(data->sense == SENSE_CHARGER) ||
				(data->sense == SENSE_IDLOW_CHARGER) ||
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
				(data->usb_accy == CPCAP_ACCY_2WIRE) ||
#endif
				(data->sense & CPCAP_BIT_SESSVLD_S))) {
				/* Wakeup device from Suspend especially when
				 * you are coming from dipping voltage[<4.2V]
				 * to higher one [4.6V - VBUS,5V]
				 */
				if (!(wake_lock_active(&data->wake_lock)))
					wake_lock(&data->wake_lock);

				notify_accy(data, CPCAP_ACCY_CHARGER);
				/* VBUS is valid and also session valid bit
				 * is set hence, we notify that charger is
				 * connected
				 */
				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
				data->state = FINISH2WIRE;
				schedule_delayed_work(&data->work,
					msecs_to_jiffies(500));
#else
				data->state = CONFIG;
#endif
			} else if ((!isVBusValid) &&
				((!(data->sense & CPCAP_BIT_SESSVLD_S) ||
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
				(data->usb_accy == CPCAP_ACCY_2WIRE) ||
#endif
				(!(data->sense & CPCAP_BIT_VBUSVLD_S))))) {
				/* Condition when the USB charger is connected &
				 * for some reason Voltage falls below the 4.4V
				 * threshold. Since USB is connected, we reset
				 * the State Machine and wait for the voltage to
				 * reach the high threshold
				 */
				data->state = CONFIG;
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
				data->usb_accy = CPCAP_ACCY_NONE;
				if (gpio_is_valid(data->twowire_data.gpio))
					gpio_set_value(data->twowire_data.gpio,
						0);
#endif

				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
				cpcap_irq_unmask(data->cpcap,
						CPCAP_IRQ_VBUSVLD);
				cpcap_irq_unmask(data->cpcap,
						CPCAP_IRQ_CHRG_DET);

				cpcap_irq_mask(data->cpcap, CPCAP_IRQ_VBUSVLD);
				schedule_delayed_work(&data->work, 0);
			}
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
		} else if ((data->sense == SENSE_2WIRE) &&
			   (data->usb_accy == CPCAP_ACCY_NONE)) {
			/* wait 750ms with GPIO low to force idle state */
			if (gpio_is_valid(data->twowire_data.gpio)) {
				gpio_set_value(data->twowire_data.gpio, 0);
				data->state = START2WIRE;
				schedule_delayed_work(&data->work,
						      msecs_to_jiffies(750));
			} else {
				printk(KERN_ERR "Detected 2wire charger but "
					"GPIO is not configured\n");
				data->state = CONFIG;
				cpcap_irq_unmask(data->cpcap,
					CPCAP_IRQ_CHRG_DET);
				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_DPI);
				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_DMI);
				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
				cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
			}
#endif
		} else if ((data->sense & CPCAP_BIT_VBUSVLD_S) &&
				(data->usb_accy == CPCAP_ACCY_NONE)) {
			data->state = CONFIG;
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_DPI);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_DMI);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
		} else {
			notify_accy(data, CPCAP_ACCY_NONE);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_CURR1);

			/* When a charger is unpowered by unplugging from the
			 * wall, VBUS voltage will drop below CHRG_DET (3.5V)
			 * until the ICHRG bits are cleared.  Once ICHRG is
			 * cleared, VBUS will rise above CHRG_DET, but below
			 * VBUSVLD (4.4V) briefly as it decays.  If the charger
			 * is re-powered while VBUS is within this window, the
			 * VBUSVLD interrupt is needed to trigger charger
			 * detection.
			 *
			 * VBUSVLD must be masked before going into suspend.
			 * See cpcap_usb_det_suspend() for details.
			 */
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_VBUSVLD);
		}
		break;

	case USB:
		get_sense(data);

		if ((data->sense & CPCAP_BIT_SE1_S) ||
			(data->sense & CPCAP_BIT_ID_GROUND_S)) {
				data->state = CONFIG;
				schedule_delayed_work(&data->work, 0);
		} else if (!(data->sense & CPCAP_BIT_VBUSVLD_S)) {
			if (data->undetect_cnt++ < UNDETECT_TRIES) {
				cpcap_irq_mask(data->cpcap, CPCAP_IRQ_CHRG_DET);
				cpcap_irq_mask(data->cpcap,
					       CPCAP_IRQ_CHRG_CURR1);
				cpcap_irq_mask(data->cpcap, CPCAP_IRQ_SE1);
				cpcap_irq_mask(data->cpcap, CPCAP_IRQ_IDGND);
				data->state = USB;
				schedule_delayed_work(&data->work,
						      msecs_to_jiffies(100));
			} else {
				data->state = CONFIG;
				schedule_delayed_work(&data->work, 0);
			}
		} else {
			data->state = USB;
			data->undetect_cnt = 0;
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_CURR1);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
		}
		break;

	case FACTORY:
		get_sense(data);

		/* The removal of a factory cable can only be detected if a
		 * charger is attached.
		 */
		if (data->sense & CPCAP_BIT_SE1_S) {
#ifdef CONFIG_TTA_CHARGER
			enable_tta();
#endif
			data->state = CONFIG;
			schedule_delayed_work(&data->work, 0);
		} else {
			data->state = FACTORY;
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
		}
		break;
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
	case START2WIRE:
		sessvalid = (data->sense & CPCAP_BIT_SESSVLD_S);
		memcpy(data->twowire_data.data, handshake,
			TWOWIRE_HANDSHAKE_LEN);
		data->twowire_data.pos = 5;
		data->twowire_data.state = CPCAP_TWOWIRE_RUNNING;
		next_time = ktime_set(0, TWOWIRE_DELAY);
		hrtimer_start(&data->hr_timer, next_time, HRTIMER_MODE_REL);

		while (sessvalid && data->twowire_data.state !=
			CPCAP_TWOWIRE_DONE) {
			msleep(10);
			get_sense(data);
			sessvalid = (data->sense & CPCAP_BIT_SESSVLD_S);
		}

		if (sessvalid && data->twowire_data.state ==
			CPCAP_TWOWIRE_DONE) {
			data->usb_accy = CPCAP_ACCY_2WIRE;
			data->state = IDENTIFY;
			schedule_delayed_work(&data->work, 0);
		} else {
			printk(KERN_ERR "2wire removed durring handshake\n");
			hrtimer_cancel(&data->hr_timer);
			if (gpio_is_valid(data->twowire_data.gpio))
				gpio_set_value(data->twowire_data.gpio, 0);
			data->state = CONFIG;
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_DPI);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_DMI);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_SE1);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
		}
		break;
	case FINISH2WIRE:
		if (gpio_is_valid(data->twowire_data.gpio))
			gpio_set_value(data->twowire_data.gpio, 0);
		data->state = CONFIG;
		break;
#endif
	default:
		/* This shouldn't happen.  Need to reset state machine. */
		vusb_disable(data);
		data->state = CONFIG;
		schedule_delayed_work(&data->work, 0);
		break;
	}
}

static void int_handler(enum cpcap_irqs int_event, void *data)
{
	struct cpcap_usb_det_data *usb_det_data = data;
	schedule_delayed_work(&(usb_det_data->work), 0);
}

static int cpcap_usb_det_probe(struct platform_device *pdev)
{
	int retval;
	struct cpcap_usb_det_data *data;
#ifdef CONFIG_CHARGER_CPCAP_2WIRE
	struct cpcap_platform_data *platform_data;
#endif

	if (pdev->dev.platform_data == NULL) {
		dev_err(&pdev->dev, "no platform_data\n");
		return -EINVAL;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->cpcap = pdev->dev.platform_data;
	data->state = CONFIG;
	platform_set_drvdata(pdev, data);
	INIT_DELAYED_WORK(&data->work, detection_work);
	data->usb_accy = CPCAP_ACCY_NONE;
	wake_lock_init(&data->wake_lock, WAKE_LOCK_SUSPEND, "usb");
	data->undetect_cnt = 0;

	data->regulator = regulator_get(NULL, "vusb");
	if (IS_ERR(data->regulator)) {
		dev_err(&pdev->dev, "Could not get regulator for cpcap_usb\n");
		retval = PTR_ERR(data->regulator);
		goto free_mem;
	}
	regulator_set_voltage(data->regulator, 3300000, 3300000);

	retval = cpcap_irq_register(data->cpcap, CPCAP_IRQ_CHRG_DET,
				    int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_CHRG_CURR1,
				     int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_SE1,
				     int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_IDGND,
				     int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_VBUSVLD,
				     int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_IDFLOAT,
				     int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_DPI,
				     int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_DMI,
				     int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_SESSVLD,
				     int_handler, data);

	/* Now that HW initialization is done, give USB control via ULPI. */
	retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC3,
				     0, CPCAP_BIT_ULPI_SPI_SEL);

#ifdef CONFIG_CHARGER_CPCAP_2WIRE
	hrtimer_init(&(data->hr_timer), CLOCK_REALTIME, HRTIMER_MODE_REL);
	data->hr_timer.function = &cpcap_send_2wire_sendbit;
	if (data->cpcap->spi && data->cpcap->spi->controller_data) {
		platform_data = data->cpcap->spi->controller_data;
		data->twowire_data.gpio = platform_data->twowire_hndshk_gpio;
	} else {
		data->twowire_data.gpio = -1;
		dev_err(&pdev->dev, "SPI platform_data missing\n");
		retval = -EINVAL;
	}
#endif

	if (retval != 0) {
		dev_err(&pdev->dev, "Initialization Error\n");
		retval = -ENODEV;
		goto free_irqs;
	}

	dev_info(&pdev->dev, "CPCAP USB detection device probed\n");

	/* Perform initial detection */
	detection_work(&(data->work.work));

	return 0;

free_irqs:
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_VBUSVLD);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDGND);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_SE1);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_CHRG_CURR1);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_CHRG_DET);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDFLOAT);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_DPI);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_DMI);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_SESSVLD);
	regulator_put(data->regulator);
free_mem:
	wake_lock_destroy(&data->wake_lock);
	kfree(data);

	return retval;
}

static int cpcap_usb_det_remove(struct platform_device *pdev)
{
	struct cpcap_usb_det_data *data = platform_get_drvdata(pdev);

	cpcap_irq_free(data->cpcap, CPCAP_IRQ_CHRG_DET);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_CHRG_CURR1);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_SE1);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDGND);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_VBUSVLD);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDFLOAT);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_SESSVLD);

	configure_hardware(data, CPCAP_ACCY_NONE);
	cancel_delayed_work_sync(&data->work);

	if ((data->usb_accy != CPCAP_ACCY_NONE) && (data->usb_dev != NULL))
		platform_device_del(data->usb_dev);

	vusb_disable(data);
	regulator_put(data->regulator);

	wake_lock_destroy(&data->wake_lock);

	kfree(data);
	return 0;
}

#ifdef CONFIG_PM
static int cpcap_usb_det_suspend(struct platform_device *pdev,
				 pm_message_t state)
{
	struct cpcap_usb_det_data *data = platform_get_drvdata(pdev);

	/* VBUSVLD cannot be unmasked when entering suspend. If left
	 * unmasked, a false interrupt will be received, keeping the
	 * device out of suspend. The interrupt does not need to be
	 * unmasked when resuming from suspend since the use case
	 * for having the interrupt unmasked is over.
	 */
	cpcap_irq_mask(data->cpcap, CPCAP_IRQ_VBUSVLD);

	return 0;
}
#else
#define cpcap_usb_det_suspend NULL
#endif

static struct platform_driver cpcap_usb_det_driver = {
	.probe		= cpcap_usb_det_probe,
	.remove		= cpcap_usb_det_remove,
	.suspend	= cpcap_usb_det_suspend,
	.driver		= {
		.name	= "cpcap_usb_det",
		.owner	= THIS_MODULE,
	},
};

static int __init cpcap_usb_det_init(void)
{
	return cpcap_driver_register(&cpcap_usb_det_driver);
}
/* The CPCAP USB detection driver must be started later to give the MUSB
 * driver time to complete its initialization. */
late_initcall(cpcap_usb_det_init);

static void __exit cpcap_usb_det_exit(void)
{
	platform_driver_unregister(&cpcap_usb_det_driver);
}
module_exit(cpcap_usb_det_exit);

MODULE_ALIAS("platform:cpcap_usb_det");
MODULE_DESCRIPTION("CPCAP USB detection driver");
MODULE_LICENSE("GPL");
