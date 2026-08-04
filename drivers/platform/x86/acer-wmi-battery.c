/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * acer-wmi-battery.c: Acer battery health control driver
 */

#include "acpi/acexcep.h"
#include <linux/device.h>
#include <linux/unaligned.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/version.h>
#include <linux/wmi.h>

MODULE_DESCRIPTION("Acer battery wmi driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Saúl Valdelvira <saul@saulv.es>");
#define WMI_GUID "79772EC5-04B1-4bfd-843C-61E7F77B6CC9"
MODULE_ALIAS("wmi:" WMI_GUID);

#define ACER_BATTERY_INDEX 1
#define SBS_CODE_TEMPERATURE 8
#define WMI_GET_BATTERY_INFO    19
#define WMI_GET_HEALTH_STATUS   20
#define WMI_SET_HEALTH_CONTROL  21

struct acer_battery_data {
	bool health_mode;
	bool calibration_mode;
};

static acpi_status get_battery_information(struct wmi_device *dev, u32 index, u32 battery, u32 *result)
{
	u32 args[2] = { index, battery };
	struct acpi_buffer input = { sizeof(args), args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmidev_evaluate_method(dev, 0, WMI_GET_BATTERY_INFO, &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	int ret = AE_OK;

	obj = output.pointer;
	if (!obj) {
		return AE_ERROR;
	} else if (obj->type != ACPI_TYPE_BUFFER) {
		ret = AE_ERROR;
		goto cleanup;
	} else if (obj->buffer.length != sizeof(u32)) {
		ret = AE_ERROR;
		goto cleanup;
	}

	*result = get_unaligned_le32(obj->buffer.pointer);
cleanup:
	kfree(obj);
	return ret;
}

enum battery_mode { HEALTH_MODE = 1, CALIBRATION_MODE = 2 };

static acpi_status update_battery_health_control_status(struct wmi_device *dev)
{
	struct input {
		u8 uBatteryNo;
		u8 uFunctionQuery;
		u8 uReserved[2];
	} __packed;

	struct output {
		u8 uFunctionList;
		u8 uReturn[2];
		u8 uFunctionStatus[5];
	} __packed;

	union acpi_object *obj;
	acpi_status status;

	/* Acer Care Center seems to always call the WMI method
	   with fixed parameters. This yields information about
	   the availability and state of both health and
	   calibration mode. The modes probably apply to
	   all batteries of the system - if there are
	   Acer laptops with multiple batteries? */
	struct input params = {
		.uBatteryNo = ACER_BATTERY_INDEX,
		.uFunctionQuery = 0x1,
	};
	struct output ret;

	struct acpi_buffer input = {
		sizeof(struct input), &params
	};

	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmidev_evaluate_method(dev, 0, WMI_GET_HEALTH_STATUS, &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		pr_err("WMI battery status call returned an acpi object"
			"that is not of buffer type\n");
		status = AE_ERROR;
		goto cleanup;
	} else if (obj->buffer.length != 8) {
		pr_err("WMI battery status call returned a buffer of "
		       "unexpected length %d\n", obj->buffer.length);
		status = AE_ERROR;
		goto cleanup;
	}

	ret = *((struct output *)obj->buffer.pointer);

	struct acer_battery_data *data = dev_get_drvdata(&dev->dev);
	if (ret.uFunctionList & HEALTH_MODE)
		data->health_mode = ret.uFunctionStatus[0];
	if (ret.uFunctionList & CALIBRATION_MODE)
		data->calibration_mode = ret.uFunctionStatus[1];
cleanup:
	kfree(obj);
	return status;
}


static acpi_status set_battery_health_control(struct wmi_device *dev, u8 function, bool function_status)
{
	struct input {
		u8 uBatteryNo;
		u8 uFunctionMask;
		u8 uFunctionStatus;
		u8 uReservedIn[5];
	} __packed;

	struct output {
		u8 uReturn;
		u8 uReservedOut;
	} __packed;

	union acpi_object *obj;
	acpi_status status;

	/* Cf. comment regarding constant argument values in
	   get_battery_health_control_status. */
	struct input params = {
		.uBatteryNo = ACER_BATTERY_INDEX,
		.uFunctionMask = function,
		.uFunctionStatus = (u8)function_status,
		.uReservedIn = { 0x0, 0x0, 0x0, 0x0, 0x0 }
	};
	struct output ret;

	struct acpi_buffer input = {
		sizeof(struct input), &params
	};

	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	status = wmidev_evaluate_method(dev, 0, WMI_SET_HEALTH_CONTROL, &input, &output);

	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;

	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		pr_err("WMI battery status set operation returned "
			"an acpi object that is not of buffer type\n");
		status = AE_ERROR;
		goto cleanup;
	} else if (obj->buffer.length != 4) {
		pr_err("WMI battery status set operation returned "
			"a buffer of unexpected length %d\n",
			obj->buffer.length);
		status = AE_ERROR;
		goto cleanup;
	}

	ret = *((struct output *)obj->buffer.pointer);
	if (ret.uReturn != 0) {
		pr_err("Operation failed with code %d\n", ret.uReturn);
		status = AE_ERROR;
		goto cleanup;
	}

	struct acer_battery_data *data = dev_get_drvdata(&dev->dev);
	switch (function) {
	case CALIBRATION_MODE:
		data->calibration_mode = function_status;
		break;
	case HEALTH_MODE:
		data->health_mode = function_status;
		break;
	}

cleanup:
	kfree(obj);
	return status;
}

static ssize_t temperature_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	acpi_status status;
	u32 value;

	status = get_battery_information(to_wmi_device(dev), SBS_CODE_TEMPERATURE, ACER_BATTERY_INDEX, &value);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (value > U16_MAX)
		return -ENXIO;

	return sysfs_emit(buf, "%d.%d", value / 10, value % 10);
}

static DEVICE_ATTR_RO(temperature);

static ssize_t do_mode_show(char *buf, bool val)
{
	int len = sprintf(buf, "%d\n", val);
	if (len <= 0)
		pr_err("Invalid sprintf len: %d\n", len);
	return len;
}

static ssize_t do_mode_store(struct device *dev, bool *attr, int mode, const char *buf, size_t nbytes)
{
	bool param_val;
	int err;

	err = kstrtobool(buf, &param_val);
	if (err)
		return err;

	if (*attr == param_val)
		return nbytes;

	acpi_status acpiret = set_battery_health_control(to_wmi_device(dev), mode, param_val);
	if (ACPI_FAILURE(acpiret))
		return -1;

	*attr = param_val;

	return nbytes;
}

static ssize_t health_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	struct acer_battery_data *data = dev_get_drvdata(dev);
	return do_mode_show(buf, data->health_mode);
}

static ssize_t health_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t nbytes)
{
	(void)attr;
	struct acer_battery_data *data = dev_get_drvdata(dev);
	return do_mode_store(dev, &data->health_mode, HEALTH_MODE, buf, nbytes);
}

static DEVICE_ATTR_RW(health_mode);

static ssize_t calibration_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	struct acer_battery_data *data = dev_get_drvdata(dev);
	return do_mode_show(buf, data->calibration_mode);
}

static ssize_t calibration_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t nbytes)
{
	(void)attr;
	struct acer_battery_data *data = dev_get_drvdata(dev);
	return do_mode_store(dev, &data->calibration_mode, CALIBRATION_MODE, buf, nbytes);
}
static DEVICE_ATTR_RW(calibration_mode);

static struct attribute *acer_wmi_battery_attrs[] = {
	&dev_attr_temperature.attr,
	&dev_attr_health_mode.attr,
	&dev_attr_calibration_mode.attr,
	NULL
};

ATTRIBUTE_GROUPS(acer_wmi_battery);

static int acer_battery_probe(struct wmi_device *wdev, const void *id)
{
	(void)id;

	struct acer_battery_data *data;
	int ret;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, data);

	acpi_status acpi_ret = update_battery_health_control_status(wdev);
	if (ACPI_FAILURE(acpi_ret)) {
		dev_err(&wdev->dev, "failed polling status of battery\n");
		return acpi_ret;
	}

	ret = devm_device_add_group(&wdev->dev, &acer_wmi_battery_group);
	if (ret) {
		dev_err(&wdev->dev, "failed to create sysfs group: %d\n", ret);
		return ret;
	}

	dev_info(&wdev->dev, "Initial status: health_mode = %d, calibration_mode = %d\n", data->health_mode, data->calibration_mode);
	return 0;
}

static void acer_battery_remove(struct wmi_device *wdev)
{
    dev_info(&wdev->dev, "Acer battery wmi driver removed\n");
}

static const struct wmi_device_id acer_wmi_battery_id_table[] = {
	{ .guid_string = WMI_GUID },
	{},
};

static struct wmi_driver acer_wmi_battery_driver = {
	.driver = {
		.name = "acer-wmi-battery",
		.groups = acer_wmi_battery_groups,
	},
	.id_table = acer_wmi_battery_id_table,
	.probe = acer_battery_probe,
	.remove = acer_battery_remove,
};

module_wmi_driver(acer_wmi_battery_driver);
