/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Libbrasero-burn
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Libbrasero-burn is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Libbrasero-burn authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Libbrasero-burn. This permission is above and beyond the permissions granted
 * by the GPL license by which Libbrasero-burn is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Libbrasero-burn is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifndef _BURN_BASICS_H
#define _BURN_BASICS_H

#include <glib.h>

#include "brasero-units.h"
#include "brasero-enums.h"

G_BEGIN_DECLS

#define BRASERO_GET_BASENAME_FOR_DISPLAY(uri, name)				\
{										\
    	gchar *escaped_basename;						\
	escaped_basename = g_path_get_basename (uri);				\
    	name = g_uri_unescape_string (escaped_basename, NULL);			\
	g_free (escaped_basename);						\
}

#define BRASERO_PLUGIN_DIRECTORY		BRASERO_LIBDIR "/brasero/plugins"
#define BRASERO_PLUGIN_KEY			"/apps/brasero/config/plugins"

#define BRASERO_BURN_TMP_FILE_NAME		"brasero_tmp_XXXXXX"

#define BRASERO_MD5_FILE			".checksum.md5"
#define BRASERO_SHA1_FILE			".checksum.sha1"
#define BRASERO_SHA256_FILE			".checksum.sha256"

#define BRASERO_BURN_FLAG_ALL 0xFFFF

const gchar *
brasero_burn_action_to_string (BraseroBurnAction action);

G_END_DECLS

#endif /* _BURN_BASICS_H */
