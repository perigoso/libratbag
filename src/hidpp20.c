/*
 * HID++ 2.0 library.
 *
 * Copyright 2015 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 * Copyright 2015 Red Hat, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Based on the HID++ 2.0 documentation provided by Nestor Lopez Casado at:
 *   https://drive.google.com/folderview?id=0BxbRzx7vEV7eWmgwazJ3NUFfQ28&usp=sharing
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "hidpp20.h"
#include "libratbag.h"
#include "libratbag-hidraw.h"
#include "libratbag-util.h"
#include "libratbag-private.h"

static int
hidpp20_write_command(struct ratbag_device *device, uint8_t *cmd, int size)
{
	int res = ratbag_hidraw_output_report(device, cmd, size);

	if (res == 0)
		return 0;

	if (res < 0)
		log_error(device->ratbag, "Error: %d\n", errno);

	return res;
}

int
hidpp20_request_command(struct ratbag_device *device, union hidpp20_message *msg)
{
	struct ratbag *ratbag = device->ratbag;
	union hidpp20_message read_buffer;
	int ret;
	uint8_t hidpp_err = 0;
	size_t msg_len;

	msg_len = msg->msg.report_id == REPORT_ID_SHORT ? SHORT_MESSAGE_LENGTH : LONG_MESSAGE_LENGTH;

	log_buf_debug(ratbag, "sending: ", msg->data, msg_len);

	/* Send the message to the Device */
	ret = hidpp20_write_command(device, msg->data, msg_len);
	if (ret)
		goto out_err;

	/*
	 * Now read the answers from the device:
	 * loop until we get the actual answer or an error code.
	 */
	do {
		ret = ratbag_hidraw_read_input_report(device, read_buffer.data, LONG_MESSAGE_LENGTH);
		log_buf_debug(ratbag, " *** received: ", read_buffer.data, ret);

		if (read_buffer.msg.report_id != REPORT_ID_SHORT &&
		    read_buffer.msg.report_id != REPORT_ID_LONG)
			continue;

		/* actual answer */
		if (read_buffer.msg.sub_id == msg->msg.sub_id &&
		    read_buffer.msg.address == msg->msg.address)
			break;

		/* error */
		if (read_buffer.msg.sub_id == __ERROR_MSG &&
		    read_buffer.msg.address == msg->msg.sub_id &&
		    read_buffer.msg.parameters[0] == msg->msg.address) {
			hidpp_err = read_buffer.msg.parameters[1];
			log_error(ratbag,
				"    HID++ error from the device (%d): %s (%02x)\n",
				read_buffer.msg.device_idx,
				hidpp_errors[hidpp_err] ? hidpp_errors[hidpp_err] : "Undocumented error code",
				hidpp_err);
			break;
		}
	} while (ret > 0);

	if (ret < 0) {
		log_error(ratbag, "    USB error: %d\n", errno);
		perror("write");
		goto out_err;
	}

	if (!hidpp_err) {
		log_buf_debug(ratbag, "    received: ", read_buffer.data, ret);
		/* copy the answer for the caller */
		*msg = read_buffer;
	}

	ret = hidpp_err;

out_err:
	return ret;
}

static inline uint16_t
hidpp20_get_unaligned_u16(uint8_t *buf)
{
	return (buf[0] << 8) | buf[1];
}

/* -------------------------------------------------------------------------- */
/* 0x0000: Root                                                               */
/* -------------------------------------------------------------------------- */

#define HIDPP_PAGE_ROOT_IDX				0x00

#define CMD_ROOT_GET_FEATURE				0x08
#define CMD_ROOT_GET_PROTOCOL_VERSION			0x18

int
hidpp_root_get_feature(struct ratbag_device *device,
		       uint16_t feature,
		       uint8_t *feature_index,
		       uint8_t *feature_type,
		       uint8_t *feature_version)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = HIDPP_PAGE_ROOT_IDX,
		.msg.address = CMD_ROOT_GET_FEATURE,
		.msg.parameters[0] = feature >> 8,
		.msg.parameters[1] = feature & 0xff,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	*feature_index = msg.msg.parameters[0];
	*feature_type = msg.msg.parameters[1];
	*feature_version = msg.msg.parameters[2];

	log_debug(device->ratbag, "feature 0x%04x is at 0x%02x\n", feature, *feature_index);
	return 0;
}

int
hidpp20_root_get_protocol_version(struct ratbag_device *device,
				  unsigned *major,
				  unsigned *minor)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_SHORT,
		.msg.device_idx = 0xff,
		.msg.sub_id = HIDPP_PAGE_ROOT_IDX,
		.msg.address = CMD_ROOT_GET_PROTOCOL_VERSION,
	};

	rc = hidpp20_request_command(device, &msg);

	if (rc == ERR_INVALID_SUBID) {
		*major = 1;
		*minor = 0;
		return 0;
	}

	if (rc == 0) {
		*major = msg.msg.parameters[0];
		*minor = msg.msg.parameters[1];
	}

	return rc;
}

/* -------------------------------------------------------------------------- */
/* 0x0001: Feature Set                                                        */
/* -------------------------------------------------------------------------- */

#define CMD_FEATURE_SET_GET_COUNT			0x08
#define CMD_FEATURE_SET_GET_FEATURE_ID			0x18

static int
hidpp20_feature_set_get_count(struct ratbag_device *device, uint8_t reg)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_FEATURE_SET_GET_COUNT,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	return msg.msg.parameters[0];
}

static int
hidpp20_feature_set_get_feature_id(struct ratbag_device *device,
				   uint8_t reg,
				   uint8_t feature_index,
				   uint16_t *feature,
				   uint8_t *type)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_FEATURE_SET_GET_FEATURE_ID,
		.msg.parameters[0] = feature_index,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	*feature = hidpp20_get_unaligned_u16(msg.msg.parameters);
	*type = msg.msg.parameters[2];

	return 0;
}

int hidpp20_feature_set_get(struct ratbag_device *device,
			    struct hidpp20_feature **feature_list)
{
	uint8_t feature_index, feature_type, feature_version;
	struct hidpp20_feature *flist;
	int rc;
	uint8_t feature_count;
	unsigned int i;

	rc = hidpp_root_get_feature(device,
				    HIDPP_PAGE_FEATURE_SET,
				    &feature_index,
				    &feature_type,
				    &feature_version);
	if (rc)
		return rc;

	rc = hidpp20_feature_set_get_count(device, feature_index);
	if (rc < 0)
		return rc;

	feature_count = (uint8_t)rc;

	if (!feature_count) {
		*feature_list = NULL;
		return 0;
	}

	flist = zalloc(feature_count * sizeof(struct hidpp20_feature));
	if (!flist)
		return -ENOMEM;

	for (i = 0; i < feature_count; i++) {
		rc = hidpp20_feature_set_get_feature_id(device,
							feature_index,
							i,
							&flist[i].feature,
							&flist[i].type);
		if (rc)
			goto err;
	}

	*feature_list = flist;
	return feature_count;
err:
	free(flist);
	return rc;
}

/* -------------------------------------------------------------------------- */
/* 0x2200: Mouse Pointer Basic Optical Sensors                                */
/* -------------------------------------------------------------------------- */

#define CMD_MOUSE_POINTER_BASIC_GET_INFO		0x08

int
hidpp20_mousepointer_get_mousepointer_info(struct ratbag_device *device,
					   uint16_t *resolution,
					   uint8_t *flags)
{
	uint8_t feature_index, feature_type, feature_version;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.address = CMD_FEATURE_SET_GET_FEATURE_ID,
		.msg.parameters[0] = feature_index,
	};
	int rc;


	rc = hidpp_root_get_feature(device,
				    HIDPP_PAGE_MOUSE_POINTER_BASIC,
				    &feature_index,
				    &feature_type,
				    &feature_version);
	if (rc)
		return rc;

	msg.msg.sub_id = feature_index;

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	*resolution = hidpp20_get_unaligned_u16(msg.msg.parameters);
	*flags = msg.msg.parameters[2];

	return 0;
}

/* -------------------------------------------------------------------------- */
/* 0x2201: Adjustable DPI                                                     */
/* -------------------------------------------------------------------------- */

#define CMD_ADJUSTABLE_DPI_GET_SENSOR_COUNT		0x08
#define CMD_ADJUSTABLE_DPI_GET_SENSOR_DPI_LIST		0x18
#define CMD_ADJUSTABLE_DPI_GET_SENSOR_DPI		0x28
#define CMD_ADJUSTABLE_DPI_SET_SENSOR_DPI		0x38

static int
hidpp20_adjustable_dpi_get_count(struct ratbag_device *device, uint8_t reg)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_ADJUSTABLE_DPI_GET_SENSOR_COUNT,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	return msg.msg.parameters[0];
}

static int
hidpp20_adjustable_dpi_get_dpi_list(struct ratbag_device *device,
				    uint8_t reg,
				    struct hidpp20_sensor *sensor)
{
	int rc;
	unsigned i = 1, dpi_index = 0;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_ADJUSTABLE_DPI_GET_SENSOR_DPI_LIST,
		.msg.parameters[0] = sensor->index,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	sensor->dpi_min = 0xffff;

	sensor->index = msg.msg.parameters[0];
	while (i < LONG_MESSAGE_LENGTH - 1 &&
	       hidpp20_get_unaligned_u16(&msg.msg.parameters[i]) != 0) {
		uint16_t value = hidpp20_get_unaligned_u16(&msg.msg.parameters[i]);

		if (value > 0xe000) {
			sensor->dpi_steps = value - 0xe000;
		} else {
			sensor->dpi_min = value < sensor->dpi_min ? value : sensor->dpi_min;
			sensor->dpi_max = value > sensor->dpi_max ? value : sensor->dpi_max;
			sensor->dpi_list[dpi_index++] = value;
		}
		assert(sensor->dpi_list[dpi_index] == 0x0000);
		i += 2;
	}

	return 0;
}


static int
hidpp20_adjustable_dpi_get_dpi(struct ratbag_device *device,
			       uint8_t reg,
			       struct hidpp20_sensor *sensor)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_ADJUSTABLE_DPI_GET_SENSOR_DPI,
		.msg.parameters[0] = sensor->index,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	sensor->dpi = hidpp20_get_unaligned_u16(&msg.msg.parameters[1]);
	sensor->default_dpi = hidpp20_get_unaligned_u16(&msg.msg.parameters[3]);

	return 0;
}

int hidpp20_adjustable_dpi_get_sensors(struct ratbag_device *device,
				       struct hidpp20_sensor **sensors_list)
{
	uint8_t feature_index, feature_type, feature_version;
	struct hidpp20_sensor *s_list, *sensor;
	uint8_t num_sensors;
	unsigned i;
	int rc;


	rc = hidpp_root_get_feature(device,
				    HIDPP_PAGE_ADJUSTABLE_DPI,
				    &feature_index,
				    &feature_type,
				    &feature_version);
	if (rc)
		return rc;

	rc = hidpp20_adjustable_dpi_get_count(device, feature_index);
	if (rc < 0)
		return rc;

	num_sensors = rc;
	if (num_sensors == 0) {
		*sensors_list = NULL;
		return 0;
	}

	s_list = zalloc(num_sensors * sizeof(struct hidpp20_sensor));
	if (!s_list)
		return -ENOMEM;

	for (i = 0; i < num_sensors; i++) {
		sensor = &s_list[i];
		sensor->index = i;
		rc = hidpp20_adjustable_dpi_get_dpi_list(device,
							 feature_index,
							 sensor);
		if (rc)
			goto err;

		rc = hidpp20_adjustable_dpi_get_dpi(device, feature_index, sensor);
		if (rc)
			goto err;

		log_debug(device->ratbag,
			  "sensor %d: current dpi: %d (default: %d) min: %d max: %d steps: %d\n",
			  sensor->index,
			  sensor->dpi,
			  sensor->default_dpi,
			  sensor->dpi_min,
			  sensor->dpi_max,
			  sensor->dpi_steps);
	}

	*sensors_list = s_list;
	return num_sensors;
err:
	free(s_list);
	return rc;
}

/* -------------------------------------------------------------------------- */
/* 0x1b04: Special keys and mouse buttons                                     */
/* -------------------------------------------------------------------------- */

#define CMD_SPECIAL_KEYS_BUTTONS_GET_COUNT		0x08
#define CMD_SPECIAL_KEYS_BUTTONS_GET_INFO		0x18
#define CMD_SPECIAL_KEYS_BUTTONS_GET_REPORTING		0x28
#define CMD_SPECIAL_KEYS_BUTTONS_SET_SET_REPORTING	0x38

static const struct hidpp20_1b04_mapping hidpp20_1b04_physical_mapping[] =
{
	{ 80, "Left"},
	{ 81, "Right"},
	{ 82, "Middle"},
	{ 83, "Back"},
	{ 86, "Forward"},
	{ 195, "AppSwitchGesture"},
	{ 196, "SmartShift"},
	{ 315, "LedToggle"},
};

static const struct hidpp20_1b04_mapping hidpp20_1b04_logical_mapping[] =
{
	{ 56, "Left Click"},
	{ 57, "Right Click"},
	{ 58, "Middle Click"},
	{ 60, "Back Click"},
	{ 62, "Forward Click"},
	{ 156, "Gesture Button"},
	{ 157, "SmartShift"},
	{ 221, "LedToggle"},
};

const char *
hidpp20_1b04_get_logical_mapping(uint16_t value)
{
	const struct hidpp20_1b04_mapping *map;

	ARRAY_FOR_EACH(hidpp20_1b04_logical_mapping, map) {
		if (map->value == value)
			return map->name;
	}

	return "UNKNOWN";
}

const char *
hidpp20_1b04_get_physical_mapping(uint16_t value)
{
	const struct hidpp20_1b04_mapping *map;

	ARRAY_FOR_EACH(hidpp20_1b04_physical_mapping, map) {
		if (map->value == value)
			return map->name;
	}

	return "UNKNOWN";
}

static int
hidpp20_special_keys_buttons_get_count(struct ratbag_device *device, uint8_t reg)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_SPECIAL_KEYS_BUTTONS_GET_COUNT,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	return msg.msg.parameters[0];
}

static int
hidpp20_special_keys_buttons_get_info(struct ratbag_device *device,
				    uint8_t reg,
				    struct hidpp20_control_id *control)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_SPECIAL_KEYS_BUTTONS_GET_INFO,
		.msg.parameters[0] = control->index,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	control->control_id = hidpp20_get_unaligned_u16(&msg.msg.parameters[0]);
	control->task_id = hidpp20_get_unaligned_u16(&msg.msg.parameters[2]);
	control->flags = msg.msg.parameters[4];
	control->position = msg.msg.parameters[5];
	control->group = msg.msg.parameters[6];
	control->group_mask = msg.msg.parameters[7];
	control->raw_XY = msg.msg.parameters[8] & 0x01;

	return 0;
}


static int
hidpp20_special_keys_buttons_get_reporting(struct ratbag_device *device,
					   uint8_t reg,
					   struct hidpp20_control_id *control)
{
	int rc;
	union hidpp20_message msg = {
		.msg.report_id = REPORT_ID_LONG,
		.msg.device_idx = 0xff,
		.msg.sub_id = reg,
		.msg.address = CMD_SPECIAL_KEYS_BUTTONS_GET_REPORTING,
		.msg.parameters[0] = control->control_id >> 8,
		.msg.parameters[1] = control->control_id & 0xff,
	};

	rc = hidpp20_request_command(device, &msg);
	if (rc)
		return rc;

	control->reporting.remapped = hidpp20_get_unaligned_u16(&msg.msg.parameters[3]);
	control->reporting.raw_XY = !!(msg.msg.parameters[2] & 0x10);
	control->reporting.persist = !!(msg.msg.parameters[2] & 0x04);
	control->reporting.divert = !!(msg.msg.parameters[2] & 0x01);

	return 0;
}

int hidpp20_special_key_mouse_get_controls(struct ratbag_device *device,
					   struct hidpp20_control_id **controls_list)
{
	uint8_t feature_index, feature_type, feature_version;
	struct hidpp20_control_id *c_list, *control;
	uint8_t num_controls;
	unsigned i;
	int rc;


	rc = hidpp_root_get_feature(device,
				    HIDPP_PAGE_SPECIAL_KEYS_BUTTONS,
				    &feature_index,
				    &feature_type,
				    &feature_version);
	if (rc)
		return rc;

	rc = hidpp20_special_keys_buttons_get_count(device, feature_index);
	if (rc < 0)
		return rc;

	num_controls = rc;
	if (num_controls == 0) {
		*controls_list = NULL;
		return 0;
	}

	c_list = zalloc(num_controls * sizeof(struct hidpp20_control_id));
	if (!c_list)
		return -ENOMEM;

	for (i = 0; i < num_controls; i++) {
		control = &c_list[i];
		control->index = i;
		rc = hidpp20_special_keys_buttons_get_info(device,
							   feature_index,
							   control);
		if (rc)
			goto err;

		rc = hidpp20_special_keys_buttons_get_reporting(device,
								feature_index,
								control);
		if (rc)
			goto err;

		log_info(device->ratbag,
			  "control %d: cid: '%s' (%d) tid: '%s' (%d) flags: 0x%02x pos: %d group: %d gmask: 0x%02x raw_XY: %s\n"
			  "      reporting: raw_xy: %s persist: %s divert: %s remapped: '%s' (%d)\n",
			  control->index,
			  hidpp20_1b04_get_physical_mapping(control->control_id),
			  control->control_id,
			  hidpp20_1b04_get_logical_mapping(control->task_id),
			  control->task_id,
			  control->flags,
			  control->position,
			  control->group,
			  control->group_mask,
			  control->raw_XY ? "yes" : "no",
			  control->reporting.raw_XY ? "yes" : "no",
			  control->reporting.persist ? "yes" : "no",
			  control->reporting.divert ? "yes" : "no",
			  hidpp20_1b04_get_logical_mapping(control->reporting.remapped),
			  control->reporting.remapped);
	}

	*controls_list = c_list;
	return num_controls;
err:
	free(c_list);
	return rc;
}
