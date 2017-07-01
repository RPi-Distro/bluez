/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arpa/inet.h>

#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>

#include "log.h"
#include "attrib-server.h"

#include "att.h"
#include "example.h"

/* FIXME: Not defined by SIG? UUID128? */
#define OPCODES_SUPPORTED_UUID          0xA001
#define BATTERY_STATE_SVC_UUID		0xA002
#define BATTERY_STATE_UUID		0xA003
#define THERM_HUMIDITY_SVC_UUID		0xA004
#define MANUFACTURER_SVC_UUID		0xA005
#define TEMPERATURE_UUID		0xA006
#define FMT_CELSIUS_UUID		0xA007
#define FMT_OUTSIDE_UUID		0xA008
#define RELATIVE_HUMIDITY_UUID		0xA009
#define FMT_PERCENT_UUID		0xA00A
#define BLUETOOTH_SIG_UUID		0xA00B
#define MANUFACTURER_NAME_UUID		0xA00C
#define MANUFACTURER_SERIAL_UUID	0xA00D
#define VENDOR_SPECIFIC_SVC_UUID	0xA00E
#define VENDOR_SPECIFIC_TYPE_UUID	0xA00F
#define FMT_KILOGRAM_UUID		0xA010
#define FMT_HANGING_UUID		0xA011

static int register_attributes(void)
{
	const char *devname = "Example Device";
	const char *desc_out_temp = "Outside Temperature";
	const char *desc_out_hum = "Outside Relative Humidity";
	const char *desc_weight = "Rucksack Weight";
	const char *manufacturer_name1 = "ACME Temperature Sensor";
	const char *manufacturer_name2 = "ACME Weighing Scales";
	const char *serial1 = "237495-3282-A";
	const char *serial2 = "11267-2327A00239";
	const unsigned char char_weight_uuid[] = { 0x80, 0x88, 0xF2, 0x18, 0x90,
		0x2C, 0x45, 0x0B, 0xB6, 0xC4, 0x62, 0x89, 0x1E, 0x8C, 0x25,
		0xE9 };
	const unsigned char prim_weight_uuid[] = { 0x4F, 0x0A, 0xC0, 0x96, 0x35,
		0xD4, 0x49, 0x11, 0x96, 0x31, 0xDE, 0xA8, 0xDC, 0x74, 0xEE,
		0xFE };
	uint8_t atval[256];
	uuid_t uuid;
	int len;
	uint16_t u16;

	/* GAP service: primary service definition */
	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	u16 = htons(GENERIC_ACCESS_PROFILE_ID);
	atval[0] = u16 >> 8;
	atval[1] = u16;
	attrib_db_add(0x0001, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* GAP service: device name characteristic */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(GATT_CHARAC_DEVICE_NAME);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x06;
	atval[2] = 0x00;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0004, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* GAP service: device name attribute */
	sdp_uuid16_create(&uuid, GATT_CHARAC_DEVICE_NAME);
	len = strlen(devname);
	strncpy((char *) atval, devname, len);
	attrib_db_add(0x0006, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	/* GATT service: primary service definition */
	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	u16 = htons(GENERIC_ATTRIB_PROFILE_ID);
	atval[0] = u16 >> 8;
	atval[1] = u16;
	attrib_db_add(0x0010, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* GATT service: attributes opcodes characteristic */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(OPCODES_SUPPORTED_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x12;
	atval[2] = 0x00;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0011, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* GATT service: attribute opcodes supported */
	sdp_uuid16_create(&uuid, OPCODES_SUPPORTED_UUID);
	atval[0] = 0xFF;
	atval[1] = 0x01;
	attrib_db_add(0x0012, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* Battery state service: primary service definition */
	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	u16 = htons(BATTERY_STATE_SVC_UUID);
	atval[0] = u16 >> 8;
	atval[1] = u16;
	attrib_db_add(0x0100, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* Battery: battery state characteristic */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(BATTERY_STATE_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x10;
	atval[2] = 0x01;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0106, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Battery: battery state attribute */
	sdp_uuid16_create(&uuid, BATTERY_STATE_UUID);
	atval[0] = 0x04;
	attrib_db_add(0x0110, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 1);

	/* Battery: Client Characteristic Configuration */
	sdp_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	atval[0] = 0x00;
	atval[1] = 0x00;
	attrib_db_add(0x0111, &uuid, ATT_NONE, ATT_AUTHENTICATION, atval, 2);

	/* Thermometer: primary service definition */
	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	u16 = htons(THERM_HUMIDITY_SVC_UUID);
	atval[0] = u16 >> 8;
	atval[1] = u16;
	attrib_db_add(0x0200, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* Thermometer: Include */
	sdp_uuid16_create(&uuid, GATT_INCLUDE_UUID);
	u16 = htons(MANUFACTURER_SVC_UUID);
	atval[0] = 0x00;
	atval[1] = 0x05;
	atval[2] = 0x04;
	atval[3] = 0x05;
	atval[4] = u16 >> 8;
	atval[5] = u16;
	attrib_db_add(0x0201, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 6);

	/* Thermometer: Include */
	atval[0] = 0x50;
	atval[1] = 0x05;
	atval[2] = 0x68;
	atval[3] = 0x05;
	attrib_db_add(0x0202, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 4);

	/* Thermometer: temperature characteristic */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(TEMPERATURE_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x04;
	atval[2] = 0x02;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0203, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Thermometer: temperature characteristic value */
	sdp_uuid16_create(&uuid, TEMPERATURE_UUID);
	atval[0] = 0x8A;
	atval[1] = 0x02;
	attrib_db_add(0x0204, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* Thermometer: temperature characteristic format */
	sdp_uuid16_create(&uuid, GATT_CHARAC_FMT_UUID);
	u16 = htons(FMT_CELSIUS_UUID);
	atval[0] = 0x0E;
	atval[1] = 0xFE;
	atval[2] = u16 >> 8;
	atval[3] = u16;
	atval[4] = 0x01;
	u16 = htons(FMT_OUTSIDE_UUID);
	atval[5] = u16 >> 8;
	atval[6] = u16;
	attrib_db_add(0x0205, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 7);

	/* Thermometer: characteristic user description */
	sdp_uuid16_create(&uuid, GATT_CHARAC_USER_DESC_UUID);
	len = strlen(desc_out_temp);
	strncpy((char *) atval, desc_out_temp, len);
	attrib_db_add(0x0206, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	/* Thermometer: relative humidity characteristic */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(RELATIVE_HUMIDITY_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x12;
	atval[2] = 0x02;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0210, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Thermometer: relative humidity value */
	sdp_uuid16_create(&uuid, RELATIVE_HUMIDITY_UUID);
	atval[0] = 0x27;
	attrib_db_add(0x0212, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 1);

	/* Thermometer: relative humidity characteristic format */
	sdp_uuid16_create(&uuid, GATT_CHARAC_FMT_UUID);
	u16 = htons(FMT_PERCENT_UUID);
	atval[0] = 0x04;
	atval[1] = 0x00;
	atval[2] = u16 >> 8;
	atval[3] = u16;
	u16 = htons(BLUETOOTH_SIG_UUID);
	atval[4] = u16 >> 8;
	atval[5] = u16;
	u16 = htons(FMT_OUTSIDE_UUID);
	atval[6] = u16 >> 8;
	atval[7] = u16;
	attrib_db_add(0x0213, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 8);

	/* Thermometer: characteristic user description */
	sdp_uuid16_create(&uuid, GATT_CHARAC_USER_DESC_UUID);
	len = strlen(desc_out_hum);
	strncpy((char *) atval, desc_out_hum, len);
	attrib_db_add(0x0214, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	/* Secondary Service: Manufacturer Service */
	sdp_uuid16_create(&uuid, GATT_SND_SVC_UUID);
	u16 = htons(MANUFACTURER_SVC_UUID);
	atval[0] = u16 >> 8;
	atval[1] = u16;
	attrib_db_add(0x0500, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* Manufacturer name characteristic definition */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(MANUFACTURER_NAME_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x02;
	atval[2] = 0x05;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0501, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Manufacturer name characteristic value */
	sdp_uuid16_create(&uuid, MANUFACTURER_NAME_UUID);
	len = strlen(manufacturer_name1);
	strncpy((char *) atval, manufacturer_name1, len);
	attrib_db_add(0x0502, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	/* Manufacturer serial number characteristic */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(MANUFACTURER_SERIAL_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x04;
	atval[2] = 0x05;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0503, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Manufacturer serial number characteristic value */
	sdp_uuid16_create(&uuid, MANUFACTURER_SERIAL_UUID);
	len = strlen(serial1);
	strncpy((char *) atval, serial1, len);
	attrib_db_add(0x0504, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	/* Secondary Service: Manufacturer Service */
	sdp_uuid16_create(&uuid, GATT_SND_SVC_UUID);
	u16 = htons(MANUFACTURER_SVC_UUID);
	atval[0] = u16 >> 8;
	atval[1] = u16;
	attrib_db_add(0x0505, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* Manufacturer name characteristic definition */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(MANUFACTURER_NAME_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x07;
	atval[2] = 0x05;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0506, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Secondary Service: Vendor Specific Service */
	sdp_uuid16_create(&uuid, GATT_SND_SVC_UUID);
	u16 = htons(VENDOR_SPECIFIC_SVC_UUID);
	atval[0] = u16 >> 8;
	atval[1] = u16;
	attrib_db_add(0x0550, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 2);

	/* Vendor Specific Type characteristic definition */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(VENDOR_SPECIFIC_TYPE_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x68;
	atval[2] = 0x05;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0560, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Vendor Specific Type characteristic value */
	sdp_uuid16_create(&uuid, VENDOR_SPECIFIC_TYPE_UUID);
	atval[0] = 0x56;
	atval[1] = 0x65;
	atval[2] = 0x6E;
	atval[3] = 0x64;
	atval[4] = 0x6F;
	atval[5] = 0x72;
	attrib_db_add(0x0568, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 6);

	/* Manufacturer name attribute */
	sdp_uuid16_create(&uuid, MANUFACTURER_NAME_UUID);
	len = strlen(manufacturer_name2);
	strncpy((char *) atval, manufacturer_name2, len);
	attrib_db_add(0x0507, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	/* Characteristic: serial number */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	u16 = htons(MANUFACTURER_SERIAL_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x09;
	atval[2] = 0x05;
	atval[3] = u16 >> 8;
	atval[4] = u16;
	attrib_db_add(0x0508, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 5);

	/* Serial number characteristic value */
	sdp_uuid16_create(&uuid, MANUFACTURER_SERIAL_UUID);
	len = strlen(serial2);
	strncpy((char *) atval, serial2, len);
	attrib_db_add(0x0509, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	/* Weight service: primary service definition */
	sdp_uuid16_create(&uuid, GATT_PRIM_SVC_UUID);
	memcpy(atval, prim_weight_uuid, 16);
	attrib_db_add(0x0680, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 16);

	/* Weight: include */
	sdp_uuid16_create(&uuid, GATT_INCLUDE_UUID);
	u16 = htons(MANUFACTURER_SVC_UUID);
	atval[0] = 0x05;
	atval[1] = 0x05;
	atval[2] = 0x09;
	atval[3] = 0x05;
	atval[4] = u16 >> 8;
	atval[5] = u16;
	attrib_db_add(0x0681, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 6);

	/* Weight: characteristic */
	sdp_uuid16_create(&uuid, GATT_CHARAC_UUID);
	atval[0] = ATT_CHAR_PROPER_READ;
	atval[1] = 0x83;
	atval[2] = 0x06;
	memcpy(atval + 3, char_weight_uuid, 16);
	attrib_db_add(0x0682, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 19);

	/* Weight: characteristic value */
	sdp_uuid128_create(&uuid, char_weight_uuid);
	atval[0] = 0x82;
	atval[1] = 0x55;
	atval[2] = 0x00;
	atval[3] = 0x00;
	attrib_db_add(0x0683, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 4);

	/* Weight: characteristic format */
	sdp_uuid16_create(&uuid, GATT_CHARAC_FMT_UUID);
	u16 = htons(FMT_KILOGRAM_UUID);
	atval[0] = 0x08;
	atval[1] = 0xFD;
	atval[2] = u16 >> 8;
	atval[3] = u16;
	u16 = htons(BLUETOOTH_SIG_UUID);
	atval[4] = u16 >> 8;
	atval[5] = u16;
	u16 = htons(FMT_HANGING_UUID);
	atval[6] = u16 >> 8;
	atval[7] = u16;
	attrib_db_add(0x0684, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, 8);

	/* Weight: characteristic user description */
	sdp_uuid16_create(&uuid, GATT_CHARAC_USER_DESC_UUID);
	len = strlen(desc_weight);
	strncpy((char *) atval, desc_weight, len);
	attrib_db_add(0x0685, &uuid, ATT_NONE, ATT_NOT_PERMITTED, atval, len);

	return 0;
}

int server_example_init(void)
{
	return register_attributes();
}

void server_example_exit(void)
{
}
