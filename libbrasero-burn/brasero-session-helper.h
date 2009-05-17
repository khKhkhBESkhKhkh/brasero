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

#ifndef _BURN_SESSION_HELPER_H_
#define _BURN_SESSION_HELPER_H_

#include <glib.h>

#include "brasero-media.h"
#include "brasero-drive.h"

#include "brasero-session.h"

G_BEGIN_DECLS

/**
 * Some convenience functions used internally
 */

BraseroMedia
brasero_burn_session_get_dest_media (BraseroBurnSession *session);

BraseroDrive *
brasero_burn_session_get_src_drive (BraseroBurnSession *session);

BraseroMedium *
brasero_burn_session_get_src_medium (BraseroBurnSession *session);

gboolean
brasero_burn_session_is_dest_file (BraseroBurnSession *session);

gboolean
brasero_burn_session_same_src_dest_drive (BraseroBurnSession *session);

#define BRASERO_BURN_SESSION_EJECT(session)					\
(brasero_burn_session_get_flags ((session)) & BRASERO_BURN_FLAG_EJECT)

#define BRASERO_BURN_SESSION_CHECK_SIZE(session)				\
(brasero_burn_session_get_flags ((session)) & BRASERO_BURN_FLAG_CHECK_SIZE)

#define BRASERO_BURN_SESSION_NO_TMP_FILE(session)				\
(brasero_burn_session_get_flags ((session)) & BRASERO_BURN_FLAG_NO_TMP_FILES)

#define BRASERO_BURN_SESSION_OVERBURN(session)					\
(brasero_burn_session_get_flags ((session)) & BRASERO_BURN_FLAG_OVERBURN)

#define BRASERO_BURN_SESSION_APPEND(session)					\
(brasero_burn_session_get_flags ((session)) & (BRASERO_BURN_FLAG_APPEND|BRASERO_BURN_FLAG_MERGE))

BraseroBurnResult
brasero_burn_session_get_tmp_image (BraseroBurnSession *session,
				    BraseroImageFormat format,
				    gchar **image,
				    gchar **toc,
				    GError **error);

BraseroBurnResult
brasero_burn_session_get_tmp_file (BraseroBurnSession *session,
				   const gchar *suffix,
				   gchar **path,
				   GError **error);

BraseroBurnResult
brasero_burn_session_get_tmp_dir (BraseroBurnSession *session,
				  gchar **path,
				  GError **error);

G_END_DECLS

#endif
