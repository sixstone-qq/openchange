/*
   OpenChange MAPI implementation.

   Copyright (C) Enrique J. Hernandez 2016

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "libmapi/libmapi.h"
#include "libmapi/libmapi_private.h"
#include <time.h>

/**
   \file mapidebug.c

   \brief Functions for displaying various data structures, mainly for debugging
 */

/**
   Dump to log a property + its value using ':' as separator

   \param prop_tag the property tag
   \param value the value whose type is defined by prop_tag
   \param level log level to dump
   \param fmt_string the format string to dump
   \param va variable list of arguments before dumping the property value

   \note it requires OC_DUMP_PROP_VALUES defined in compilation time. If not,
   then no dump is done

   \return the result of the operation for the caller to check if out is correct
*/
_PUBLIC_ enum MAPISTATUS mapidebug_dump_property(uint32_t prop_tag, const void *value, uint8_t level, const char *fmt_string, ...)
{
#ifdef OC_DUMP_PROP_VALUES
	char		*out_str, *s;
	enum MAPISTATUS retval;
	TALLOC_CTX	*local_mem_ctx;
	va_list		ap;

	local_mem_ctx = talloc_new(NULL);
	OPENCHANGE_RETVAL_IF(!local_mem_ctx, MAPI_E_NOT_ENOUGH_MEMORY, NULL);

	retval = mapidebug_property(local_mem_ctx, prop_tag, value, &out_str);
	if (retval == MAPI_E_SUCCESS) {
		va_start(ap, fmt_string);
		vasprintf(&s, fmt_string, ap);
		va_end(ap);
		OC_DEBUG(level, "%s%s", s, out_str);
	}

	talloc_free(local_mem_ctx);
	return retval;
#else
        return MAPI_E_SUCCESS;
#endif
}

/**
   Output in out as string allocated in mem_ctx of a property + its
   value using ':' as separator.

   \param mem_ctx the memory context where out is allocated
   \param prop_tag the property tag
   \param value the value whose type is defined by prop_tag
   \param out the string in format "prop_tag: prop_value"

   \return the result of the operation for the caller to check if out is correct
*/
_PUBLIC_ enum MAPISTATUS mapidebug_property(TALLOC_CTX *mem_ctx, uint32_t prop_tag,
					    const void *value, char **out)
{
	struct BinaryArray_r	   *bin_array;
	const char		   *prop_tag_str;
	char			   *result;
	struct FILETIME		   *time;
	int			   i;
	struct mapi_MV_LONG_STRUCT *long_array;
	NTTIME			   nt_time;
	struct StringArray_r	   *str_array;
	struct StringArrayW_r	   *str_w_array;

	OPENCHANGE_RETVAL_IF(!out, MAPI_E_INVALID_PARAMETER, NULL);

	prop_tag_str = get_proptag_name(prop_tag);
	if (prop_tag_str) {
		result = talloc_asprintf(mem_ctx, "%s: ", prop_tag_str);
	} else {
		result = talloc_asprintf(mem_ctx, "0x%.8x: ", prop_tag);
	}
	OPENCHANGE_RETVAL_IF(!result, MAPI_E_NOT_ENOUGH_MEMORY, NULL);

	if (!value) {
		result = talloc_asprintf_append(result, "(NULL)");
		goto end;
	}

	switch (prop_tag & 0xFFFF) {
	case PT_SHORT:
		result = talloc_asprintf_append(result, "0x%x", (*(const uint16_t *)value));
		break;
	case PT_LONG:
	case PT_OBJECT:
		result = talloc_asprintf_append(result, "%u", (*(const uint32_t *)value));
		break;
	case PT_DOUBLE:
		result = talloc_asprintf_append(result, "%f", (*(const double *)value));
		break;
	case PT_BOOLEAN:
		result = talloc_asprintf_append(result, "0x%x", (*(const uint8_t *)value));
		break;
	case PT_I8:
		result = talloc_asprintf_append(result, "%.16"PRIx64, (*(const uint64_t *)value));
		break;
	case PT_STRING8:
	case PT_UNICODE:
		if ((*(const uint16_t *)value) == 0x0000) {
			/* its an empty string */
			result = talloc_asprintf_append(result, "''");
		} else if ((*(enum MAPISTATUS *)value) != MAPI_E_NOT_FOUND) {
			/* its a valid string */
			result = talloc_asprintf_append(result, "'%s'", (const char *)value);
		} else {
			/* its a null or otherwise problematic string */
			result = talloc_asprintf_append(result, "(NULL)");
		}
		break;
	case PT_SYSTIME:
		time = (struct FILETIME *)value;
		nt_time = time->dwHighDateTime;
		nt_time <<= 32;
		nt_time |= time->dwLowDateTime;
		result = talloc_asprintf_append(result, "%s", nt_time_string(mem_ctx, nt_time));
		break;
	case PT_ERROR:
		result = talloc_asprintf_append(result, "(ERROR) 0x%.8x", (*(const uint32_t *)value));
		break;
	case PT_CLSID:
	{
		result = talloc_asprintf_append(result, "%s", GUID_string(mem_ctx, (struct GUID *)value));
		break;
	}
	case PT_SVREID:
	case PT_BINARY:
		/* TODO */
		dump_data(0, ((const struct Binary_r *)value)->lpb, ((const struct Binary_r *)value)->cb);
		break;
	case PT_MV_LONG:
		long_array = (struct mapi_MV_LONG_STRUCT *)value;
		for (i = 0; i < long_array->cValues - 1; i++) {
			result = talloc_asprintf_append(result, "0x%.8x, ", long_array->lpl[i]);
			OPENCHANGE_RETVAL_IF(!result, MAPI_E_NOT_ENOUGH_MEMORY, NULL);
		}
		result = talloc_asprintf_append(result, "0x%.8x", long_array->lpl[i]);
		break;
	case PT_MV_STRING8:
		str_array = (struct StringArray_r *)value;
		for (i = 0; i < str_array->cValues - 1; i++) {
			result = talloc_asprintf_append(result, "%s, ", str_array->lppszA[i]);
			OPENCHANGE_RETVAL_IF(!result, MAPI_E_NOT_ENOUGH_MEMORY, NULL);
		}
		result = talloc_asprintf_append(result, "%s", str_array->lppszA[i]);
		break;
	case PT_MV_UNICODE:
		str_w_array = (struct StringArrayW_r *)value;
		for (i = 0; i < str_w_array->cValues - 1; i++) {
			result = talloc_asprintf_append(result, "%s, ", str_w_array->lppszW[i]);
			OPENCHANGE_RETVAL_IF(!result, MAPI_E_NOT_ENOUGH_MEMORY, NULL);
		}
		result = talloc_asprintf_append(result, "%s", str_w_array->lppszW[i]);
		break;
	case PT_MV_BINARY:
		bin_array = (struct BinaryArray_r *) value;
		result = talloc_asprintf_append(result, "ARRAY(%d)", bin_array->cValues);
		OPENCHANGE_RETVAL_IF(!result, MAPI_E_NOT_ENOUGH_MEMORY, NULL);
		for (i = 0; i < bin_array->cValues; i++) {
			result = talloc_asprintf_append(result, "\tPT_MV_BINARY [%d]:", i);
			OPENCHANGE_RETVAL_IF(!result, MAPI_E_NOT_ENOUGH_MEMORY, NULL);
			/* TODO: Print bin data */
		}
		break;
	default:
		result = talloc_asprintf_append(result, "missing impl");
		break;
	}
end:

	OPENCHANGE_RETVAL_IF(!result, MAPI_E_NOT_ENOUGH_MEMORY, NULL);
	*out = result;
	return MAPI_E_SUCCESS;
}
