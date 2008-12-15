/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * brasero
 * Copyright (C) Philippe Rouquier 2005-2008 <bonfire-app@wanadoo.fr>
 * 
 *  Brasero is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 * 
 * brasero is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with brasero.  If not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifndef _BRASERO_VOLUME_H_
#define _BRASERO_VOLUME_H_

#include <glib-object.h>
#include <gio/gio.h>

#include "burn-drive.h"

G_BEGIN_DECLS

#define BRASERO_TYPE_VOLUME             (brasero_volume_get_type ())
#define BRASERO_VOLUME(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), BRASERO_TYPE_VOLUME, BraseroVolume))
#define BRASERO_VOLUME_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), BRASERO_TYPE_VOLUME, BraseroVolumeClass))
#define BRASERO_IS_VOLUME(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BRASERO_TYPE_VOLUME))
#define BRASERO_IS_VOLUME_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), BRASERO_TYPE_VOLUME))
#define BRASERO_VOLUME_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), BRASERO_TYPE_VOLUME, BraseroVolumeClass))

typedef struct _BraseroVolumeClass BraseroVolumeClass;
typedef struct _BraseroVolume BraseroVolume;

struct _BraseroVolumeClass
{
	BraseroMediumClass parent_class;
};

struct _BraseroVolume
{
	BraseroMedium parent_instance;
};

GType brasero_volume_get_type (void) G_GNUC_CONST;

BraseroVolume *
brasero_volume_new (BraseroDrive *drive,
		    const gchar *udi);

gchar *
brasero_volume_get_name (BraseroVolume *volume);

GIcon *
brasero_volume_get_icon (BraseroVolume *volume);

gboolean
brasero_volume_is_mounted (BraseroVolume *volume);

gchar *
brasero_volume_get_mount_point (BraseroVolume *volume,
				GError **error);

gboolean
brasero_volume_umount (BraseroVolume *volume,
		       gboolean wait,
		       GError **error);

gboolean
brasero_volume_mount (BraseroVolume *volume,
		      gboolean wait,
		      GError **error);

gboolean
brasero_volume_eject (BraseroVolume *volume,
		      gboolean wait,
		      GError **error);

void
brasero_volume_cancel_current_operation (BraseroVolume *volume);

G_END_DECLS

#endif /* _BRASERO_VOLUME_H_ */
