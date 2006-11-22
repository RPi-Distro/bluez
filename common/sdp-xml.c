/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2005-2006  Marcel Holtmann <marcel@holtmann.org>
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

#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <malloc.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>

#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "logging.h"

#include "sdp-xml.h"

#define STRBUFSIZE 1024
#define MAXINDENT 64

static void convert_raw_data_to_xml(sdp_data_t *value, int indent_level,
		void *data, void (*appender)(void *, const char *))
{
	int i, hex;
	char buf[STRBUFSIZE];
	char indent[MAXINDENT];
	char next_indent[MAXINDENT];

	if (!value)
		return;

	if (indent_level >= MAXINDENT)
		indent_level = MAXINDENT - 2;

	for (i = 0; i < indent_level; i++) {
		indent[i] = '\t';
		next_indent[i] = '\t';
	}

	indent[i] = '\0';
	next_indent[i] = '\t';
	next_indent[i + 1] = '\0';

	buf[STRBUFSIZE - 1] = '\0';

	switch (value->dtd) {
	case SDP_DATA_NIL:
		appender(data, indent);
		appender(data, "<nil/>\n");
		break;

	case SDP_BOOL:
		appender(data, indent);
		appender(data, "<boolean value=\"");
		appender(data, value->val.uint8 ? "true" : "false");
		appender(data, "\" />\n");
		break;

	case SDP_UINT8:
		appender(data, indent);
		appender(data, "<uint8 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "0x%02x", value->val.uint8);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_UINT16:
		appender(data, indent);
		appender(data, "<uint16 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "0x%04x", value->val.uint16);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_UINT32:
		appender(data, indent);
		appender(data, "<uint32 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "0x%08x", value->val.uint32);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_UINT64:
		appender(data, indent);
		appender(data, "<uint64 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "0x%016jx", value->val.uint64);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_UINT128:
		appender(data, indent);
		appender(data, "<uint128 value=\"");

		for (i = 0; i < 16; i++) {
			sprintf(&buf[i * 2], "%02x",
				(unsigned char) value->val.uint128.data[i]);
		}

		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_INT8:
		appender(data, indent);
		appender(data, "<int8 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "%d", value->val.int8);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_INT16:
		appender(data, indent);
		appender(data, "<int16 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "%d", value->val.int16);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_INT32:
		appender(data, indent);
		appender(data, "<int32 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "%d", value->val.int32);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_INT64:
		appender(data, indent);
		appender(data, "<int64 value=\"");
		snprintf(buf, STRBUFSIZE - 1, "%jd", value->val.int64);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_INT128:
		appender(data, indent);
		appender(data, "<int128 value=\"");

		for (i = 0; i < 16; i++) {
			sprintf(&buf[i * 2], "%02x",
				(unsigned char) value->val.int128.data[i]);
		}
		appender(data, buf);

		appender(data, "\" />\n");
		break;

	case SDP_UUID16:
		appender(data, indent);
		appender(data, "<uuid value=\"");
		snprintf(buf, STRBUFSIZE - 1, "0x%04x", value->val.uuid.value.uuid16);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_UUID32:
		appender(data, indent);
		appender(data, "<uuid value=\"");
		snprintf(buf, STRBUFSIZE - 1, "0x%08x", value->val.uuid.value.uuid32);
		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_UUID128:
		appender(data, indent);
		appender(data, "<uuid value=\"");

		snprintf(buf, STRBUFSIZE - 1,
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[0],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[1],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[2],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[3],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[4],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[5],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[6],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[7],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[8],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[9],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[10],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[11],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[12],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[13],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[14],
			 (unsigned char) value->val.uuid.value.
			 uuid128.data[15]);

		appender(data, buf);
		appender(data, "\" />\n");
		break;

	case SDP_TEXT_STR8:
	case SDP_TEXT_STR16:
	case SDP_TEXT_STR32:
	{
		hex = 0;

		int num_chars_to_escape = 0;
		
		for (i = 0; i < value->unitSize; i++) {
			if (i == (value->unitSize - 1)
			    && value->val.str[i] == '\0')
				break;
			if (!isprint(value->val.str[i])) {
				hex = 1;
				break;
			}
			
			/* XML is evil, must do this... */
			if ((value->val.str[i] == '<') ||
			    (value->val.str[i] == '>') ||
			    (value->val.str[i] == '"') ||
			    (value->val.str[i] == '&'))
			    num_chars_to_escape++;
			
		}
		
		appender(data, indent);

		appender(data, "<text ");

		char *strBuf = 0;

		if (hex) {
			appender(data, "encoding=\"hex\" ");
			strBuf = (char *) malloc(sizeof(char)
						 * (value->unitSize * 2 + 1));

			/* Unit Size seems to include the size for dtd
			   It is thus off by 1
			   This is safe for Normal strings, but not
			   hex encoded data */
			for (i = 0; i < (value->unitSize-1); i++)
				sprintf(&strBuf[i*sizeof(char)*2],
					"%02x",
					(unsigned char) value->val.str[i]);

			strBuf[value->unitSize * 2] = '\0';
		}
		else {
			int j;
			/* escape the XML disallowed chars */
			strBuf = (char *)
				malloc(sizeof(char) *
				(value->unitSize + 1 + num_chars_to_escape * 4));
			for (i = 0, j = 0; i < value->unitSize; i++) {
				if (value->val.str[i] == '&') {
					strBuf[j++] = '&';
					strBuf[j++] = 'a';
					strBuf[j++] = 'm';
					strBuf[j++] = 'p';
				}
				else if (value->val.str[i] == '<') {
					strBuf[j++] = '&';
					strBuf[j++] = 'l';
					strBuf[j++] = 't';
				}
				else if (value->val.str[i] == '>') {
					strBuf[j++] = '&';
					strBuf[j++] = 'g';
					strBuf[j++] = 't';
				}
				else if (value->val.str[i] == '"') {
					strBuf[j++] = '&';
					strBuf[j++] = 'q';
					strBuf[j++] = 'u';
					strBuf[j++] = 'o';
					strBuf[j++] = 't';
				}
				else {
					strBuf[j++] = value->val.str[i];
				}
			}

			strBuf[j] = '\0';
		}

		appender(data, "value=\"");
		appender(data, strBuf);
		appender(data, "\" />\n");
		free(strBuf);
		break;
	}

	case SDP_URL_STR8:
	case SDP_URL_STR16:
	case SDP_URL_STR32:
		appender(data, indent);
		appender(data, "<url value=\"");
		appender(data, value->val.str);
		appender(data, "\" />\n");
		break;

	case SDP_SEQ8:
	case SDP_SEQ16:
	case SDP_SEQ32:
		appender(data, indent);
		appender(data, "<sequence>\n");

		convert_raw_data_to_xml(value->val.dataseq,
					indent_level + 1, data, appender);
					
		appender(data, indent);
		appender(data, "</sequence>\n");
		
		break;

	case SDP_ALT8:
	case SDP_ALT16:
	case SDP_ALT32:
		appender(data, indent);

		appender(data, "<alternate>\n");

		convert_raw_data_to_xml(value->val.dataseq,
					indent_level + 1, data, appender);
		appender(data, indent);

		appender(data, "</alternate>\n");
	       
		break;
	}

	convert_raw_data_to_xml(value->next, indent_level, data, appender);
}

struct conversion_data {
	void *data;
	void (*appender)(void *data, const char *);
};

static void convert_raw_attr_to_xml_func(void *val, void *data)
{
	struct conversion_data *cd = (struct conversion_data *) data;
	sdp_data_t *value = (sdp_data_t *) val;
	char buf[STRBUFSIZE];

	buf[STRBUFSIZE - 1] = '\0';
	snprintf(buf, STRBUFSIZE - 1, "\t<attribute id=\"0x%04x\">\n",
		 value->attrId);
	cd->appender(cd->data, buf);

	if (data)
		convert_raw_data_to_xml(value, 2, cd->data, cd->appender);
	else
		cd->appender(cd->data, "\t\tNULL\n");

	cd->appender(cd->data, "\t</attribute>\n");
}

/*
 * Will convert the sdp record to XML.  The appender and data can be used
 * to control where to output the record (e.g. file or a data buffer).  The
 * appender will be called repeatedly with data and the character buffer
 * (containing parts of the generated XML) to append.
 */
void convert_sdp_record_to_xml(sdp_record_t *rec,
			void *data, void (*appender)(void *, const char *))
{
	struct conversion_data cd;

	cd.data = data;
	cd.appender = appender;

	if (rec && rec->attrlist) {
		appender(data, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n\n");
		appender(data, "<record>\n");
		sdp_list_foreach(rec->attrlist,
				 convert_raw_attr_to_xml_func, &cd);
		appender(data, "</record>\n");
	}
}

static sdp_data_t *sdp_xml_parse_uuid128(const char *data)
{
	uint128_t val;
	int i;
	int j;

	char buf[3];

	memset(&val, 0, sizeof(val));

	buf[2] = '\0';

	for (j = 0, i = 0; i < strlen(data);) {
		if (data[i] == '-') {
			i++;
			continue;
		}

		buf[0] = data[i];
		buf[1] = data[i + 1];

		val.data[j++] = strtoul(buf, 0, 16);
		i += 2;
	}

	return sdp_data_alloc(SDP_UUID128, &val);
}

sdp_data_t *sdp_xml_parse_uuid(const char *data)
{
	int len;
	char *endptr;
	uint32_t val;
	uint16_t val2 = val;

	len = strlen(data);

	if (len == 36)
		return sdp_xml_parse_uuid128(data);

	val = strtoll(data, &endptr, 16);

	/* Couldn't parse */
	if (*endptr != '\0')
		return NULL;

	if (val > USHRT_MAX)
		return sdp_data_alloc(SDP_UUID32, &val);

	return sdp_data_alloc(SDP_UUID16, &val2);

	/* Should never get here */
	return NULL;
}

sdp_data_t *sdp_xml_parse_int(const char * data, uint8_t dtd)
{
	char *endptr;
	sdp_data_t *ret = NULL;

	switch (dtd) {
	case SDP_BOOL:
	{
		uint8_t val = 0;

		if (!strcmp("true", data)) {
			val = 1;
		}
		
		else if (!strcmp("false", data)) {
			val = 0;
		}
		else {
			return NULL;
		}
		
		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_INT8:
	{
		int8_t val = strtoul(data, &endptr, 0);

       		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;		

		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_UINT8:
	{
		uint8_t val = strtoul(data, &endptr, 0);

		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;
		
		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_INT16:
	{
		int16_t val = strtoul(data, &endptr, 0);

		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;
	
		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_UINT16:
	{
		uint16_t val = strtoul(data, &endptr, 0);

       		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;		

		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_INT32:
	{
		int32_t val = strtoul(data, &endptr, 0);

       		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;		
		
		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_UINT32:
	{
		uint32_t val = strtoul(data, &endptr, 0);

       		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;		

		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_INT64:
	{
		int64_t val = strtoull(data, &endptr, 0);
		
       		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;		
		
		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_UINT64:
	{
		uint64_t val = strtoull(data, &endptr, 0);

       		/* Failed to parse */
		if ((endptr != data) && (*endptr != '\0'))
			return NULL;		

		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	case SDP_INT128:
	case SDP_UINT128:
	{
		uint128_t val;
		int i = 0;
		char buf[3];

		buf[2] = '\0';

		for (; i < 32; i += 2) {
			buf[0] = data[i];
			buf[1] = data[i + 1];

			val.data[i] = strtoul(buf, 0, 16);
		}

		ret = sdp_data_alloc(dtd, &val);
		break;
	}

	};

	return ret;
}

static sdp_data_t *sdp_xml_parse_url_with_size(const char *data, uint8_t dtd)
{
	return sdp_data_alloc(dtd, data);
}

sdp_data_t *sdp_xml_parse_url(const char *data)
{
	uint8_t dtd = SDP_URL_STR8;

	if (strlen(data) > UCHAR_MAX)
		dtd = SDP_URL_STR16;

	return sdp_xml_parse_url_with_size(data, dtd);
}

static char *sdp_xml_parse_text_decode(const char *data, char encoding, uint32_t *length)
{
	int len = strlen(data);
	char *text;

	if (encoding == SDP_XML_ENCODING_NORMAL) {
		text = strdup(data);
		*length = len;
	} else {
		char buf[3], *decoded;
		int i;

		decoded = malloc((len >> 1) + 1);

		/* Ensure the string is a power of 2 */
		len = (len >> 1) << 1;

		buf[2] = '\0';

		for (i = 0; i < len; i += 2) {
			buf[0] = data[i];
			buf[1] = data[i + 1];

			decoded[i >> 1] = strtoul(buf, 0, 16);
		}

		decoded[len >> 1] = '\0';
		text = decoded;
		*length = len >> 1;
	}

	return text;
}

#if 0
static sdp_data_t *sdp_xml_parse_text_with_size(const char *data, char encoding, uint8_t dtd)
{
	char *text;
	uint32_t length;
	sdp_data_t *ret;

	text = sdp_xml_parse_text_decode(data, encoding, &length);
	ret = sdp_data_alloc_with_length(dtd, text, length);

	debug("Unit size %d length %d: -->%s<--\n", ret->unitSize, length, text);

	return ret;
}
#endif

sdp_data_t *sdp_xml_parse_text(const char *data, char encoding)
{
	uint8_t dtd = SDP_TEXT_STR8;
	char *text;
	uint32_t length;
	sdp_data_t *ret;

	text = sdp_xml_parse_text_decode(data, encoding, &length);

	if (length > UCHAR_MAX)
		dtd = SDP_TEXT_STR16;

	ret = sdp_data_alloc_with_length(dtd, text, length);

	debug("Unit size %d length %d: -->%s<--\n", ret->unitSize, length, text);

	return ret;
}

sdp_data_t *sdp_xml_parse_nil(const char *data)
{
	return sdp_data_alloc(SDP_DATA_NIL, 0);
}
