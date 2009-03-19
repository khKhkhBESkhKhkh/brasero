/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Libbrasero-media
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Libbrasero-media is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Libbrasero-media authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Libbrasero-media. This permission is above and beyond the permissions granted
 * by the GPL license by which Libbrasero-media is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Libbrasero-media is distributed in the hope that it will be useful,
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gdk/gdk.h>

#include "brasero-enums.h"

#include "brasero-media-private.h"

#include "brasero-medium.h"
#include "brasero-drive.h"

#include "scsi-device.h"
#include "scsi-mmc1.h"
#include "scsi-mmc2.h"
#include "scsi-mmc3.h"
#include "scsi-spc1.h"
#include "scsi-utils.h"
#include "scsi-mode-pages.h"
#include "scsi-status-page.h"
#include "scsi-write-page.h"
#include "scsi-q-subchannel.h"
#include "scsi-dvd-structures.h"
#include "burn-volume.h"


const gchar *types [] = {	N_("File"),
				N_("CDROM"),
				N_("CD-R"),
				N_("CD-RW"),
				N_("DVDROM"),
				N_("DVD-R"),
				N_("DVD-RW"),
				N_("DVD+R"),
				N_("DVD+RW"),
				N_("DVD+R dual layer"),
				N_("DVD+RW dual layer"),
				N_("DVD-R dual layer"),
				N_("DVD-RAM"),
				N_("Blu-ray disc"),
				N_("Writable Blu-ray disc"),
				N_("Rewritable Blu-ray disc"),
				NULL };

typedef enum {
	BRASERO_MEDIUM_CAP_INVALID	= 0,
	BRASERO_MEDIUM_CAP_TRUE		= 1,
	BRASERO_MEDIUM_CAP_FALSE		= 2
} BraseroMediumCapState;

typedef struct _BraseroMediumPrivate BraseroMediumPrivate;
struct _BraseroMediumPrivate
{
	GThread *probe;
	gint probe_id;

	GSList *tracks;

	const gchar *type;

	gchar *id;

	gint max_rd;
	gint max_wrt;

	gint *rd_speeds;
	gint *wr_speeds;

	gint64 block_num;
	gint64 block_size;

	guint first_open_track;
	guint64 next_wr_add;

	BraseroMedia info;
	BraseroDrive *drive;

	gchar *CD_TEXT_title;

	/* Do we really need both? */
	guint dummy_sao:2;
	guint dummy_tao:2;
	guint burnfree:2;

	guint probe_cancelled:1;
};

#define BRASERO_MEDIUM_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), BRASERO_TYPE_MEDIUM, BraseroMediumPrivate))

/**
 * Try to open the drive exclusively but don't block; if drive can't be opened
 * exclusively then retry every second until we're shut or the drive state
 * changes to not busy.
 * No exclusive at the moment since when the medium is mounted we can't use excl
 */

#define BUSY_RETRY_TIME			1000

typedef enum {
	BRASERO_MEDIUM_TRACK_NONE		= 0,
	BRASERO_MEDIUM_TRACK_DATA		= 1,
	BRASERO_MEDIUM_TRACK_AUDIO		= 1 << 1,
	BRASERO_MEDIUM_TRACK_COPY		= 1 << 2,
	BRASERO_MEDIUM_TRACK_PREEMP		= 1 << 3,
	BRASERO_MEDIUM_TRACK_4_CHANNELS		= 1 << 4,
	BRASERO_MEDIUM_TRACK_INCREMENTAL	= 1 << 5,
	BRASERO_MEDIUM_TRACK_LEADOUT		= 1 << 6
} BraseroMediumTrackType;

typedef struct _BraseroMediumTrack BraseroMediumTrack;

struct _BraseroMediumTrack {
	guint session;
	BraseroMediumTrackType type;
	guint64 start;
	guint64 blocks_num;
};

enum
{
	PROP_0,
	PROP_DRIVE,
};

enum {
	PROBED,
	LAST_SIGNAL
};
static gulong medium_signals [LAST_SIGNAL] = {0, };

#define BRASERO_MEDIUM_OPEN_ATTEMPTS			5

static GObjectClass* parent_class = NULL;

/**
 * This one is not supposed to be public API. It's declared in burn-caps.c
 */

BraseroBurnFlag
brasero_medium_supported_flags (BraseroMedium *self,
				BraseroBurnFlag flags)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* This is always FALSE */
	if (priv->info & BRASERO_MEDIUM_PLUS)
		flags &= ~BRASERO_BURN_FLAG_DUMMY;
	/* Simulation is only possible according to write modes. This mode is
	 * mostly used by cdrecord/wodim for CLONE images. */
	else if (priv->info & BRASERO_MEDIUM_DVD) {
		if (priv->dummy_sao != BRASERO_MEDIUM_CAP_TRUE)
			flags &= ~BRASERO_BURN_FLAG_DUMMY;
	}
	else if (flags & BRASERO_BURN_FLAG_DAO) {
		if (priv->dummy_sao != BRASERO_MEDIUM_CAP_TRUE)
			flags &= ~BRASERO_BURN_FLAG_DUMMY;
	}
	else if (priv->dummy_tao != BRASERO_MEDIUM_CAP_TRUE)
		flags &= ~BRASERO_BURN_FLAG_DUMMY;

	if (!priv->burnfree)
		flags &= ~BRASERO_BURN_FLAG_BURNPROOF;

	return flags;
}

/**
 * This one is not supposed to be public API. It's declared in burn-caps.c
 */

gboolean
brasero_medium_support_flags (BraseroMedium *self,
			      BraseroBurnFlag flags)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	if (flags & BRASERO_BURN_FLAG_DUMMY) {
		/* This is always FALSE */
		if (priv->info & BRASERO_MEDIUM_PLUS)
			return FALSE;

		if (priv->info & BRASERO_MEDIUM_DVD) {
			if (priv->dummy_sao != BRASERO_MEDIUM_CAP_TRUE)
				return FALSE;
		}
		else if (flags & BRASERO_BURN_FLAG_DAO) {
			if (priv->dummy_sao != BRASERO_MEDIUM_CAP_TRUE)
				return FALSE;
		}
		else if (priv->dummy_tao != BRASERO_MEDIUM_CAP_TRUE)
			return FALSE;
	}

	if (flags & BRASERO_BURN_FLAG_BURNPROOF) {
		if (!priv->burnfree)
			return FALSE;
	}

	return TRUE;
}

/**
 * brasero_medium_get_tooltip:
 * @medium: #BraseroMedium
 *
 * Returns a tooltip to be displayed in the UI.
 * It is of the form {content type} {disc type} in {drive name}.
 *
 * Return value: a #gchar *.
 *
 **/
gchar *
brasero_medium_get_tooltip (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;
	BraseroDrive *drive;
	BraseroMedia media;
	const gchar *type;
	gchar *label;
	gchar *name;

	g_return_val_if_fail (medium != NULL, NULL);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), NULL);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	media = brasero_medium_get_status (BRASERO_MEDIUM (medium));
	if (media & BRASERO_MEDIUM_FILE) {
		/* Translators: This is a fake drive, a file, and means that
		 * when we're writing, we're writing to a file and create an
		 * image on the hard drive. */
		return g_strdup (_("Image File"));
	}

	type = brasero_medium_get_type_string (BRASERO_MEDIUM (medium));
	drive = brasero_medium_get_drive (BRASERO_MEDIUM (medium));
	name = brasero_drive_get_display_name (drive);

	if (media & BRASERO_MEDIUM_BLANK) {
		/* NOTE for translators: the first %s is the disc type and the
		 * second %s the name of the drive this disc is in. */
		label = g_strdup_printf (_("Blank %s in %s"),
					 type,
					 name);
	}
	else if (BRASERO_MEDIUM_IS (media, BRASERO_MEDIUM_HAS_AUDIO|BRASERO_MEDIUM_HAS_DATA)) {
		/* NOTE for translators: the first %s is the disc type and the
		 * second %s the name of the drive this disc is in. */
		label = g_strdup_printf (_("Audio and data %s in %s"),
					 type,
					 name);
	}
	else if (media & BRASERO_MEDIUM_HAS_AUDIO) {
		/* NOTE for translators: the first %s is the disc type and the
		 * second %s the name of the drive this disc is in. */
		label = g_strdup_printf (_("Audio %s in %s"),
					 type,
					 name);
	}
	else if (media & BRASERO_MEDIUM_HAS_DATA) {
		/* NOTE for translators: the first %s is the disc type and the
	 	* second %s the name of the drive this disc is in. */
		label = g_strdup_printf (_("Data %s in %s"),
					 type,
					 name);
	}
	else {
		/* NOTE for translators: the first %s is the disc type and the
	 	* second %s the name of the drive this disc is in. */
		label = g_strdup_printf (_("%s in %s"),
					 type,
					 name);
	}

	g_free (name);
	return label;
}

/**
 * brasero_medium_get_type_string:
 * @medium: #BraseroMedium
 *
 * Returns the medium type as a string to be displayed in a UI.
 *
 * Return value: a #gchar *.
 *
 **/
const gchar *
brasero_medium_get_type_string (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;

	g_return_val_if_fail (medium != NULL, NULL);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), NULL);

	priv = BRASERO_MEDIUM_PRIVATE (medium);
	return priv->type;
}

/**
 * brasero_medium_get_status:
 * @medium: #BraseroMedium
 *
 * Gets the medium type and state.
 *
 * Return value: a #BraseroMedia.
 *
 **/
BraseroMedia
brasero_medium_get_status (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;

	if (!medium)
		return BRASERO_MEDIUM_NONE;

	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), BRASERO_MEDIUM_NONE);

	priv = BRASERO_MEDIUM_PRIVATE (medium);
	return priv->info;
}

/**
 * brasero_medium_get_last_data_track_address:
 * @medium: #BraseroMedium
 * @byte: a #gint64 * or NULL
 * @sector: a #gint64 * or NULL
 *
 * Stores in either @byte (in bytes) or in @sector (in blocks) the address where
 * the last session starts. This is useful when creating a multisession image or
 * when reading the contents of this last track.
 *
 **/
gboolean
brasero_medium_get_last_data_track_address (BraseroMedium *medium,
					    gint64 *byte,
					    gint64 *sector)
{
	GSList *iter;
	BraseroMediumPrivate *priv;
	BraseroMediumTrack *track = NULL;

	g_return_val_if_fail (medium != NULL, FALSE);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), FALSE);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	for (iter = priv->tracks; iter; iter = iter->next) {
		BraseroMediumTrack *current;

		current = iter->data;
		if (current->type & BRASERO_MEDIUM_TRACK_DATA)
			track = current;
	}

	if (!track) {
		if (byte)
			*byte = -1;
		if (sector)
			*sector = -1;
		return FALSE;
	}

	if (byte)
		*byte = track->start * priv->block_size;

	if (sector)
		*sector = track->start;

	return TRUE;
}

/**
 * brasero_medium_get_last_data_track_space:
 * @medium: #BraseroMedium
 * @size: a #gint64 * or NULL
 * @blocks: a #gint64 * or NULL
 *
 * Stores in either @size (in bytes) or in @blocks (in blocks) the space used by
 * the last track on the medium.
 *
 **/
gboolean
brasero_medium_get_last_data_track_space (BraseroMedium *medium,
					  gint64 *size,
					  gint64 *blocks)
{
	GSList *iter;
	BraseroMediumPrivate *priv;
	BraseroMediumTrack *track = NULL;

	g_return_val_if_fail (medium != NULL, FALSE);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), FALSE);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	for (iter = priv->tracks; iter; iter = iter->next) {
		BraseroMediumTrack *current;

		current = iter->data;
		if (current->type & BRASERO_MEDIUM_TRACK_DATA)
			track = current;
	}

	if (!track) {
		if (size)
			*size = -1;
		if (blocks)
			*blocks = -1;
		return FALSE;
	}

	if (size)
		*size = track->blocks_num * priv->block_size;
	if (blocks)
		*blocks = track->blocks_num;

	return TRUE;
}

/**
 * brasero_medium_get_track_num:
 * @medium: #BraseroMedium
 *
 * Gets the number of tracks on the medium.
 *
 * Return value: a #guint.
 *
 **/
guint
brasero_medium_get_track_num (BraseroMedium *medium)
{
	GSList *iter;
	guint retval = 0;
	BraseroMediumPrivate *priv;

	g_return_val_if_fail (medium != NULL, 0);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), 0);

	priv = BRASERO_MEDIUM_PRIVATE (medium);
	for (iter = priv->tracks; iter; iter = iter->next) {
		BraseroMediumTrack *current;

		current = iter->data;
		if (current->type & BRASERO_MEDIUM_TRACK_LEADOUT)
			break;

		retval ++;
	}

	return retval;
}

static BraseroMediumTrack *
brasero_medium_get_track (BraseroMedium *medium,
			  guint num)
{
	guint i = 1;
	GSList *iter;
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	for (iter = priv->tracks; iter; iter = iter->next) {
		BraseroMediumTrack *current;

		current = iter->data;
		if (current->type == BRASERO_MEDIUM_TRACK_LEADOUT)
			break;

		if (i == num)
			return current;

		i++;
	}

	return NULL;
}

/**
 * brasero_medium_get_track_space:
 * @medium: a #BraseroMedium
 * @num: a #guint
 * @size: a #gint64 * or NULL
 * @blocks: a #gint64 * or NULL
 *
 * Stores in either @size (in bytes) or in @blocks (in blocks) the space used
 * by session @num on the disc.
 *
 * Return value: a #gboolean. Returns TRUE if information could be retrieved;
 * FALSE otherwise (usually when track @num doesn't exist).
 *
 **/
gboolean
brasero_medium_get_track_space (BraseroMedium *medium,
				guint num,
				gint64 *size,
				gint64 *blocks)
{
	BraseroMediumPrivate *priv;
	BraseroMediumTrack *track;

	g_return_val_if_fail (medium != NULL, FALSE);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), FALSE);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	track = brasero_medium_get_track (medium, num);
	if (!track) {
		if (size)
			*size = -1;
		if (blocks)
			*blocks = -1;
		return FALSE;
	}

	if (size)
		*size = track->blocks_num * priv->block_size;
	if (blocks)
		*blocks = track->blocks_num;

	return TRUE;
}

/**
 * brasero_medium_get_track_address:
 * @medium: a #BraseroMedium
 * @num: a #guint
 * @byte: a #gint64 * or NULL
 * @sector: a #gint64 * or NULL
 *
 * Stores in either @byte (in bytes) or in @sector (in blocks) the address at
 * which the session identified by @num starts.
 *
 * Return value: a #gboolean. Returns TRUE if information could be retrieved;
 * FALSE otherwise (usually when track @num doesn't exist).
 *
 **/
gboolean
brasero_medium_get_track_address (BraseroMedium *medium,
				  guint num,
				  gint64 *byte,
				  gint64 *sector)
{
	BraseroMediumPrivate *priv;
	BraseroMediumTrack *track;

	g_return_val_if_fail (medium != NULL, FALSE);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), FALSE);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	track = brasero_medium_get_track (medium, num);
	if (!track) {
		if (byte)
			*byte = -1;
		if (sector)
			*sector = -1;
		return FALSE;
	}

	if (byte)
		*byte = track->start * priv->block_size;
	if (sector)
		*sector = track->start;

	return TRUE;	
}

/**
 * brasero_medium_get_next_writable_address:
 * @medium: #BraseroMedium
 *
 * Gets the address (block number) that can be used to write a new session on @medium
 *
 * Return value: a #gint64.
 *
 **/
gint64
brasero_medium_get_next_writable_address (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;

	g_return_val_if_fail (medium != NULL, 0);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), 0);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	/* There is one exception to this with closed DVD+RW/DVD-RW restricted */
	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_PLUS)
	||  BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_RESTRICTED)
	||  BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_PLUS_DL)) {
		BraseroMediumTrack *first;

		/* These are always writable so give the next address after the 
		 * last volume. */
		if (!priv->tracks)
			return 0;

		first = priv->tracks->data;

		/* round to the nearest 16th block */
		return (((first->start + first->blocks_num) + 15) / 16) * 16;
	}

	return priv->next_wr_add;
}

/**
 * brasero_medium_get_max_write_speed:
 * @medium: #BraseroMedium
 *
 * Gets the maximum speed that can be used to write to @medium.
 * Note: the speed are in B/sec.
 *
 * Return value: a #gint64.
 *
 **/
gint64
brasero_medium_get_max_write_speed (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;

	g_return_val_if_fail (medium != NULL, 0);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), 0);

	priv = BRASERO_MEDIUM_PRIVATE (medium);
	return priv->max_wrt * 1000;
}

/**
 * brasero_medium_get_write_speeds:
 * @medium: #BraseroMedium
 *
 * Gets an array holding all possible speeds to write to @medium.
 * Note: the speed are in B/sec.
 *
 * Return value: a #gint64 *.
 *
 **/
gint64 *
brasero_medium_get_write_speeds (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;
	gint64 *speeds;
	guint max = 0;
	guint i;

	g_return_val_if_fail (medium != NULL, NULL);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), NULL);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	if (!priv->wr_speeds)
		return NULL;

	while (priv->wr_speeds [max] != 0) max ++;

	speeds = g_new0 (gint64, max + 1);

	/* NOTE: about the following, it's not KiB here but KB */
	for (i = 0; i < max; i ++)
		speeds [i] = priv->wr_speeds [i] * 1000;

	return speeds;
}

/**
 * NOTEs about the following functions:
 * for all closed media (including ROM types) capacity == size of data and 
 * should be the size of all data on the disc, free space is 0
 * for all blank -R types capacity == free space and size of data == 0
 * for all multisession -R types capacity == free space since having the real
 * capacity of the media would be useless as we can only use this type of media
 * to append more data
 * for all -RW types capacity = free space + size of data. Here they can be 
 * appended (use free space) or rewritten (whole capacity).
 *
 * Usually:
 * the free space is the size of the leadout track
 * the size of data is the sum of track sizes (excluding leadout)
 * the capacity depends on the media:
 * for closed discs == sum of track sizes
 * for multisession discs == free space (leadout size)
 * for blank discs == (free space) leadout size
 * for rewritable/blank == use SCSI functions to get capacity (see below)
 *
 * In fact we should really need the size of data in DVD+/-RW cases since the
 * session is always equal to the size of the disc. 
 */

/**
 * brasero_medium_get_data_size:
 * @medium: #BraseroMedium
 * @size: a #gint64 * or NULL
 * @blocks: a #gint64 * or NULL
 *
 * Stores in either @size (in bytes) or @blocks (the number of blocks) the size
 * used to store data (including audio on CDs) on the disc.
 *
 **/
void
brasero_medium_get_data_size (BraseroMedium *medium,
			      gint64 *size,
			      gint64 *blocks)
{
	GSList *iter;
	BraseroMediumPrivate *priv;
	BraseroMediumTrack *track = NULL;

	g_return_if_fail (medium != NULL);
	g_return_if_fail (BRASERO_IS_MEDIUM (medium));

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	if (!priv->tracks) {
		/* that's probably because it wasn't possible to retrieve info */
		if (size)
			*size = 0;

		if (blocks)
			*blocks = 0;

		return;
	}

	for (iter = priv->tracks; iter; iter = iter->next) {
		BraseroMediumTrack *tmp;

		tmp = iter->data;
		if (tmp->type == BRASERO_MEDIUM_TRACK_LEADOUT)
			break;

		track = iter->data;
	}

	if (size)
		*size = track ? (track->start + track->blocks_num) * priv->block_size: 0;

	if (blocks)
		*blocks = track ? track->start + track->blocks_num: 0;
}

/**
 * brasero_medium_get_free_space:
 * @medium: #BraseroMedium
 * @size: a #gint64 * or NULL
 * @blocks: a #gint64 * or NULL
 *
 * Stores in either @size (in bytes) or @blocks (the number of blocks) the space
 * on the disc that can be used for writing.
 *
 **/
void
brasero_medium_get_free_space (BraseroMedium *medium,
			       gint64 *size,
			       gint64 *blocks)
{
	GSList *iter;
	BraseroMediumPrivate *priv;
	BraseroMediumTrack *track = NULL;

	g_return_if_fail (medium != NULL);
	g_return_if_fail (BRASERO_IS_MEDIUM (medium));

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	if (!priv->tracks) {
		/* that's probably because it wasn't possible to retrieve info.
		 * maybe it also happens with unformatted DVD+RW */

		if (priv->info & BRASERO_MEDIUM_CLOSED) {
			if (size)
				*size = 0;

			if (blocks)
				*blocks = 0;
		}
		else {
			if (size)
				*size = priv->block_num * priv->block_size;

			if (blocks)
				*blocks = priv->block_num;
		}

		return;
	}

	for (iter = priv->tracks; iter; iter = iter->next) {
		BraseroMediumTrack *tmp;

		tmp = iter->data;
		if (tmp->type == BRASERO_MEDIUM_TRACK_LEADOUT) {
			track = iter->data;
			break;
		}
	}

	if (size) {
		if (!track) {
			/* No leadout was found so the disc is probably closed:
			 * no free space left. */
			*size = 0;
		}
		else if (track->blocks_num <= 0)
			*size = (priv->block_num - track->start) * priv->block_size;
		else
			*size = track->blocks_num * priv->block_size;
	}

	if (blocks) {
		if (!track) {
			/* No leadout was found so the disc is probably closed:
			 * no free space left. */
			*blocks = 0;
		}
		else if (track->blocks_num <= 0)
			*blocks = priv->block_num - track->blocks_num;
		else
			*blocks = track->blocks_num;
	}
}

/**
 * brasero_medium_get_capacity:
 * @medium: #BraseroMedium
 * @size: a #gint64 * or NULL
 * @blocks: a #gint64 * or NULL
 *
 * Stores in either @size (in bytes) or @blocks (the number of blocks) the total
 * disc space.
 * Note that when the disc is closed this space is the one occupied by data. 
 * Otherwise it is the sum of free and used space.
 *
 **/
void
brasero_medium_get_capacity (BraseroMedium *medium,
			     gint64 *size,
			     gint64 *blocks)
{
	BraseroMediumPrivate *priv;

	g_return_if_fail (medium != NULL);
	g_return_if_fail (BRASERO_IS_MEDIUM (medium));

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	if (priv->info & BRASERO_MEDIUM_REWRITABLE) {
		if (size)
			*size = priv->block_num * priv->block_size;

		if (blocks)
			*blocks = priv->block_num;
	}
	else  if (priv->info & BRASERO_MEDIUM_CLOSED)
		brasero_medium_get_data_size (medium, size, blocks);
	else
		brasero_medium_get_free_space (medium, size, blocks);
}

/**
 * Test presence of simulate burning
 */

static BraseroBurnResult
brasero_medium_test_simulate_CD_SAO (BraseroMedium *self,
				     BraseroDeviceHandle *handle,
				     BraseroScsiErrCode *code)
{
	BraseroScsiGetConfigHdr *hdr = NULL;
	BraseroScsiCDTAODesc *tao_desc;
	BraseroScsiFeatureDesc *desc;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* Try TAO and then SAO if it isn't persistent */
	BRASERO_MEDIA_LOG ("Checking simulate (CD TAO)");
	result = brasero_mmc2_get_configuration_feature (handle,
							 BRASERO_SCSI_FEAT_WRT_TAO,
							 &hdr,
							 &size,
							 code);
	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("GET CONFIGURATION failed");
		return BRASERO_BURN_ERR;
	}

	desc = hdr->desc;
	if (!desc->current)
		BRASERO_MEDIA_LOG ("Feature is not current");

	tao_desc = (BraseroScsiCDTAODesc *) desc->data;
	priv->dummy_tao = tao_desc->dummy ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	priv->burnfree = tao_desc->buf ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	g_free (hdr);
	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_test_simulate_CD_TAO (BraseroMedium *self,
				     BraseroDeviceHandle *handle,
				     BraseroScsiErrCode *code)
{
	BraseroScsiGetConfigHdr *hdr = NULL;
	BraseroScsiCDSAODesc *sao_desc;
	BraseroScsiFeatureDesc *desc;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	BRASERO_MEDIA_LOG ("Checking simulate (CD SAO)");
	result = brasero_mmc2_get_configuration_feature (handle,
							 BRASERO_SCSI_FEAT_WRT_SAO_RAW,
							 &hdr,
							 &size,
							 code);
	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("GET CONFIGURATION failed");
		return BRASERO_BURN_ERR;
	}

	desc = hdr->desc;
	if (!desc->current)
		BRASERO_MEDIA_LOG ("Feature is not current");

	sao_desc = (BraseroScsiCDSAODesc *) desc->data;
	priv->dummy_sao = sao_desc->dummy ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	priv->burnfree = sao_desc->buf ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	g_free (hdr);
	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_test_simulate_DVDRW (BraseroMedium *self,
				    BraseroDeviceHandle *handle,
				    BraseroScsiErrCode *code)
{
	BraseroScsiDVDRWlessWrtDesc *less_wrt_desc;
	BraseroScsiGetConfigHdr *hdr = NULL;
	BraseroScsiFeatureDesc *desc;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* Only DVD-R(W) support simulation */
	BRASERO_MEDIA_LOG ("Checking simulate (DVD-R/W)");
	result = brasero_mmc2_get_configuration_feature (handle,
							 BRASERO_SCSI_FEAT_WRT_DVD_LESS,
							 &hdr,
							 &size,
							 code);
	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("GET CONFIGURATION failed");
		return BRASERO_BURN_ERR;
	}

	desc = hdr->desc;
	if (!desc->current)
		BRASERO_MEDIA_LOG ("Feature is not current");

	less_wrt_desc = (BraseroScsiDVDRWlessWrtDesc *) desc->data;
	priv->dummy_sao = less_wrt_desc->dummy ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	priv->dummy_tao = less_wrt_desc->dummy ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	priv->burnfree = less_wrt_desc->buf ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	g_free (hdr);
	return BRASERO_BURN_OK;
}

/**
 * This is a last resort when the initialization has failed.
 */

static void
brasero_medium_test_simulate_2A (BraseroMedium *self,
				 BraseroDeviceHandle *handle,
				 BraseroScsiErrCode *code)
{
	BraseroScsiStatusPage *page_2A = NULL;
	BraseroScsiModeData *data = NULL;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size = 0;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	result = brasero_spc1_mode_sense_get_page (handle,
						   BRASERO_SPC_PAGE_STATUS,
						   &data,
						   &size,
						   code);
	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("MODE SENSE failed");
		return;
	}

	/* NOTE: this bit is only valid:
	 * - for CDs when mode write is TAO or SAO
	 * - for DVDs when mode write is incremental or SAO
	 */

	page_2A = (BraseroScsiStatusPage *) &data->page;
	priv->dummy_sao = page_2A->dummy ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	priv->dummy_tao = page_2A->dummy ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	priv->burnfree = page_2A->buffer ? BRASERO_MEDIUM_CAP_TRUE:BRASERO_MEDIUM_CAP_FALSE;
	g_free (data);
}

static void
brasero_medium_init_caps (BraseroMedium *self,
			  BraseroDeviceHandle *handle,
			  BraseroScsiErrCode *code)
{
	BraseroMediumPrivate *priv;
	BraseroScsiResult res;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* These special media don't support/need burnfree and simulation */
	if (priv->info & (BRASERO_MEDIUM_PLUS|BRASERO_MEDIUM_BD))
		return;

	if (priv->info & BRASERO_MEDIUM_CD) {
		/* we have to do both */
		res = brasero_medium_test_simulate_CD_SAO (self, handle, code);
		if (res == BRASERO_SCSI_OK)
			brasero_medium_test_simulate_CD_TAO (self, handle, code);
	}
	else
		res = brasero_medium_test_simulate_DVDRW (self, handle, code);

	BRASERO_MEDIA_LOG ("Tested simulation %d %d, burnfree %d",
			  priv->dummy_tao,
			  priv->dummy_sao,
			  priv->burnfree);

	if (res == BRASERO_SCSI_OK)
		return;

	/* it didn't work out as expected use fallback */
	BRASERO_MEDIA_LOG ("Using fallback 2A page for testing simulation and burnfree");
	brasero_medium_test_simulate_2A (self, handle, code);

	BRASERO_MEDIA_LOG ("Re-tested simulation %d %d, burnfree %d",
			  priv->dummy_tao,
			  priv->dummy_sao,
			  priv->burnfree);
}

/**
 * Function to retrieve the capacity of a media
 */

static BraseroBurnResult
brasero_medium_get_capacity_CD_RW (BraseroMedium *self,
				   BraseroDeviceHandle *handle,
				   BraseroScsiErrCode *code)
{
	BraseroScsiAtipData *atip_data = NULL;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size = 0;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	BRASERO_MEDIA_LOG ("Retrieving capacity from atip");

	result = brasero_mmc1_read_atip (handle,
					 &atip_data,
					 &size,
					 NULL);

	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("READ ATIP failed (scsi error)");
		return BRASERO_BURN_ERR;
	}

	/* check the size of the structure: it must be at least 16 bytes long */
	if (size < 16) {
		if (size)
			g_free (atip_data);

		BRASERO_MEDIA_LOG ("READ ATIP failed (wrong size)");
		return BRASERO_BURN_ERR;
	}

	priv->block_num = BRASERO_MSF_TO_LBA (atip_data->desc->leadout_mn,
					      atip_data->desc->leadout_sec,
					      atip_data->desc->leadout_frame);
	g_free (atip_data);

	BRASERO_MEDIA_LOG ("Format capacity %lli %lli",
			   priv->block_num,
			   priv->block_size);

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_get_capacity_DVD_RW (BraseroMedium *self,
				    BraseroDeviceHandle *handle,
				    BraseroScsiErrCode *code)
{
	BraseroScsiFormatCapacitiesHdr *hdr = NULL;
	BraseroScsiFormattableCapacityDesc *desc;
	BraseroScsiMaxCapacityDesc *current;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	gint i, max;
	gint size;

	BRASERO_MEDIA_LOG ("Retrieving format capacity");

	priv = BRASERO_MEDIUM_PRIVATE (self);
	result = brasero_mmc2_read_format_capacities (handle,
						      &hdr,
						      &size,
						      code);
	if (result != BRASERO_SCSI_OK) {
		g_free (hdr);

		BRASERO_MEDIA_LOG ("READ FORMAT CAPACITIES failed");
		return BRASERO_BURN_ERR;
	}

	/* NOTE: for BD-RE there is a slight problem to determine the exact
	 * capacity of the medium when it is unformatted. Indeed the final size
	 * of the User Data Area will depend on the size of the Spare areas.
	 * On the other hand if it's formatted then that's OK, just take the 
	 * current one.
	 * NOTE: that could work also for BD-R SRM+POW and BD-R RRM */

	/* see if the media is already formatted */
	current = hdr->max_caps;
	if (!(current->type & BRASERO_SCSI_DESC_FORMATTED)) {
		BRASERO_MEDIA_LOG ("Unformatted media");
		priv->info |= BRASERO_MEDIUM_UNFORMATTED;

		/* if unformatted, a DVD-RAM will return its maximum formattable
		 * size in this descriptor and that's what we're looking for. */
		if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVD_RAM)) {
			priv->block_num = BRASERO_GET_32 (current->blocks_num);
			priv->block_size = 2048;
			goto end;
		}
	}
	else if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BDRE)) {
		priv->block_num = BRASERO_GET_32 (current->blocks_num);
		priv->block_size = 2048;
		goto end;
	}

	max = (hdr->len - 
	      sizeof (BraseroScsiMaxCapacityDesc)) /
	      sizeof (BraseroScsiFormattableCapacityDesc);

	desc = hdr->desc;
	for (i = 0; i < max; i ++, desc ++) {
		/* search for the correct descriptor */
		if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_PLUS)) {
			if (desc->format_type == BRASERO_SCSI_DVDRW_PLUS) {
				priv->block_num = BRASERO_GET_32 (desc->blocks_num);
				priv->block_size = BRASERO_GET_24 (desc->type_param);

				/* that can happen */
				if (!priv->block_size)
					priv->block_size = 2048;

				break;
			}
		}
		else if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BDRE)) {
			/* This is for unformatted BDRE: since we can't know the
			 * size of the Spare Area in advance, we take the vendor
			 * preferred one. Always following are the smallest one
			 * and the biggest one. */
			if (desc->format_type == BRASERO_SCSI_BDRE_FORMAT) {
				priv->block_num = BRASERO_GET_32 (desc->blocks_num);
				break;
			}
		}
		else if (desc->format_type == BRASERO_SCSI_MAX_PACKET_SIZE_FORMAT) {
			priv->block_num = BRASERO_GET_32 (desc->blocks_num);
			break;
		}
	}

end:

	BRASERO_MEDIA_LOG ("Format capacity %lli %lli",
			  priv->block_num,
			  priv->block_size);

	g_free (hdr);
	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_get_capacity_by_type (BraseroMedium *self,
				     BraseroDeviceHandle *handle,
				     BraseroScsiErrCode *code)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* For DVDs/BDs that's always that block size */
	priv->block_size = 2048;

	if (!(priv->info & BRASERO_MEDIUM_REWRITABLE))
		return BRASERO_BURN_OK;

	if (priv->info & BRASERO_MEDIUM_CD)
		brasero_medium_get_capacity_CD_RW (self, handle, code);
	else	/* Works for BD-RE as well */
		brasero_medium_get_capacity_DVD_RW (self, handle, code);

	return BRASERO_BURN_OK;
}

/**
 * Functions to retrieve the speed
 */

static BraseroBurnResult
brasero_medium_get_speed_mmc3 (BraseroMedium *self,
			       BraseroDeviceHandle *handle,
			       BraseroScsiErrCode *code)
{
	int size;
	int num_desc, i;
	gint max_rd, max_wrt;
	BraseroScsiResult result;
	BraseroMediumPrivate *priv;
	BraseroScsiWrtSpdDesc *desc;
	BraseroScsiGetPerfData *wrt_perf = NULL;

	BRASERO_MEDIA_LOG ("Retrieving speed (Get Performance)");

	/* NOTE: this only work if there is RT streaming feature with
	 * wspd bit set to 1. At least an MMC3 drive. */
	priv = BRASERO_MEDIUM_PRIVATE (self);
	result = brasero_mmc3_get_performance_wrt_spd_desc (handle,
							    &wrt_perf,
							    &size,
							    code);

	if (result != BRASERO_SCSI_OK) {
		g_free (wrt_perf);

		BRASERO_MEDIA_LOG ("GET PERFORMANCE failed");
		return BRASERO_BURN_ERR;
	}

	num_desc = (size - sizeof (BraseroScsiGetPerfHdr)) /
		    sizeof (BraseroScsiWrtSpdDesc);

	if (num_desc <=  0)
		goto end; 

	priv->rd_speeds = g_new0 (gint, num_desc + 1);
	priv->wr_speeds = g_new0 (gint, num_desc + 1);

	max_rd = 0;
	max_wrt = 0;

	desc = (BraseroScsiWrtSpdDesc*) &wrt_perf->data;
	for (i = 0; i < num_desc; i ++, desc ++) {
		priv->rd_speeds [i] = BRASERO_GET_32 (desc->rd_speed);
		priv->wr_speeds [i] = BRASERO_GET_32 (desc->wr_speed);

		max_rd = MAX (max_rd, priv->rd_speeds [i]);
		max_wrt = MAX (max_wrt, priv->wr_speeds [i]);
	}

	priv->max_rd = max_rd;
	priv->max_wrt = max_wrt;

	BRASERO_MEDIA_LOG ("Maximum Speed (mmc3) %i", max_wrt);

end:

	g_free (wrt_perf);

	/* strangely there are so drives (I know one case) which support this
	 * function but don't report any speed. So if our top speed is 0 then
	 * use the other way to get the speed. It was a Teac */
	if (!priv->max_wrt)
		return BRASERO_BURN_ERR;

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_get_page_2A_write_speed_desc (BraseroMedium *self,
					     BraseroDeviceHandle *handle,
					     BraseroScsiErrCode *code)
{
	BraseroScsiStatusPage *page_2A = NULL;
	BraseroScsiStatusWrSpdDesc *desc;
	BraseroScsiModeData *data = NULL;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	gint desc_num, i;
	gint max_wrt = 0;
	gint max_num;
	int size = 0;

	BRASERO_MEDIA_LOG ("Retrieving speed (2A speeds)");

	priv = BRASERO_MEDIUM_PRIVATE (self);
	result = brasero_spc1_mode_sense_get_page (handle,
						   BRASERO_SPC_PAGE_STATUS,
						   &data,
						   &size,
						   code);
	if (result != BRASERO_SCSI_OK) {
		g_free (data);

		BRASERO_MEDIA_LOG ("MODE SENSE failed");
		return BRASERO_BURN_ERR;
	}

	page_2A = (BraseroScsiStatusPage *) &data->page;

	if (size < 18) {
		g_free (data);
		BRASERO_MEDIA_LOG ("wrong page size");
		return BRASERO_BURN_ERR;
	}

	priv->max_rd = BRASERO_GET_16 (page_2A->rd_max_speed);
	priv->max_wrt = BRASERO_GET_16 (page_2A->wr_max_speed);

	/* Check if we can use the speed descriptors; if not use maximum */
	if (size < 20) {
		BRASERO_MEDIA_LOG ("Maximum Speed (Page 2A [old]) %i", priv->max_wrt);

		/* also add fake speed descriptors */
		priv->wr_speeds = g_new0 (gint, 2);
		priv->wr_speeds [0] = BRASERO_GET_16 (page_2A->wr_max_speed);
		priv->rd_speeds = g_new0 (gint, 2);
		priv->rd_speeds [0] = BRASERO_GET_16 (page_2A->rd_max_speed);

		g_free (data);
		return BRASERO_BURN_OK;
	}

	desc_num = BRASERO_GET_16 (page_2A->wr_speed_desc_num);
	max_num = size -
		  sizeof (BraseroScsiStatusPage) -
		  sizeof (BraseroScsiModeHdr);
	max_num /= sizeof (BraseroScsiWrtSpdDesc);

	if (max_num < 0)
		max_num = 0;

	if (desc_num > max_num)
		desc_num = max_num;

	priv->wr_speeds = g_new0 (gint, desc_num + 1);
	desc = page_2A->wr_spd_desc;
	for (i = 0; i < desc_num; i ++, desc ++) {
		priv->wr_speeds [i] = BRASERO_GET_16 (desc->speed);
		max_wrt = MAX (max_wrt, priv->wr_speeds [i]);
	}

	if (max_wrt)
		priv->max_wrt = max_wrt;

	BRASERO_MEDIA_LOG ("Maximum Speed (Page 2A) %i", priv->max_wrt);
	g_free (data);

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_get_speed (BraseroMedium *self,
			  BraseroDeviceHandle *handle,
			  BraseroScsiErrCode *code)
{
	BraseroScsiResult result;

	BRASERO_MEDIA_LOG ("Retrieving media available speeds");

	result = brasero_medium_get_speed_mmc3 (self, handle, code);
	if (result == BRASERO_BURN_OK)
		return result;

	/* Fallback */
	result = brasero_medium_get_page_2A_write_speed_desc (self, handle, code);
	return result;
}

/**
 * Functions to get information about disc contents
 */

static BraseroBurnResult
brasero_medium_track_volume_size (BraseroMedium *self,
				  BraseroMediumTrack *track,
				  BraseroDeviceHandle *handle)
{
	BraseroMediumPrivate *priv;
	BraseroBurnResult res;
	GError *error = NULL;
	BraseroVolSrc *vol;
	gint64 nb_blocks;

	if (!track)
		return BRASERO_BURN_ERR;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* This is a special case. For DVD+RW and DVD-RW in restricted
	 * mode, there is only one session that takes the whole disc size
	 * once formatted. That doesn't necessarily means they have data
	 * Note also that they are reported as complete though you can
	 * still add data (with growisofs). It is nevertheless on the 
	 * condition that the fs is valid.
	 * So we check if their first and only volume is valid. 
	 * That's also used when the track size is reported 300 KiB
	 * see below */
	vol = brasero_volume_source_open_device_handle (handle, NULL);
	res = brasero_volume_get_size (vol,
				       track->start,
				       &nb_blocks,
				       &error);
	brasero_volume_source_close (vol);

	if (!res) {
		BRASERO_MEDIA_LOG ("Failed to retrieve the volume size: %s",
				  error && error->message ? 
				  error->message:"unknown error");

		if (error)
			g_error_free (error);

		return BRASERO_BURN_ERR;
	}

	track->blocks_num = nb_blocks;
	return BRASERO_BURN_OK;
}

static gboolean
brasero_medium_track_written_SAO (BraseroDeviceHandle *handle,
				  int track_num,
				  int track_start)
{
	unsigned char buffer [2048];
	BraseroScsiResult result;

	BRASERO_MEDIA_LOG ("Checking for TDBs in track pregap.");

	/* The two following sectors are readable */
	result = brasero_mmc1_read_block (handle,
					  TRUE,
					  BRASERO_SCSI_BLOCK_TYPE_ANY,
					  BRASERO_SCSI_BLOCK_HEADER_NONE,
					  BRASERO_SCSI_BLOCK_NO_SUBCHANNEL,
					  track_start - 1,
					  1,
					  buffer,
					  sizeof (buffer),
					  NULL);

	if (result == BRASERO_SCSI_OK) {
		int i;

		if (buffer [0] != 'T' || buffer [1] != 'D' || buffer [2] != 'I') {
			BRASERO_MEDIA_LOG ("Track was probably recorded in SAO mode - no TDB.");
			return TRUE;
		}

		/* Find the TDU (16 bytes) for the track (there can be for other tracks).
		 * i must be < 128 = ((2048 - 8 (size TDB)) / 16 (size TDU). */
		for (i = 0; i < 128; i ++) {
			if (BRASERO_GET_BCD (buffer [8 + i * 16]) != track_num)
				break;
		}

		if (i >= 128) {
			BRASERO_MEDIA_LOG ("No appropriate TDU for track");
			return TRUE;
		}

		if (buffer [8 + i * 16] == 0x80 || buffer [8 + i * 16] == 0x00) {
			BRASERO_MEDIA_LOG ("Track was recorded in TAO mode.");
			return FALSE;
		}

		BRASERO_MEDIA_LOG ("Track was recorded in Packet mode.");
		return FALSE;
	}

	BRASERO_MEDIA_LOG ("No pregap. That track must have been recorded in SAO mode.");
	return TRUE;
}

static BraseroBurnResult
brasero_medium_track_get_info (BraseroMedium *self,
			       gboolean multisession,
			       BraseroMediumTrack *track,
			       int track_num,
			       BraseroDeviceHandle *handle,
			       BraseroScsiErrCode *code)
{
	BraseroScsiTrackInfo track_info;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size;

	BRASERO_MEDIA_LOG ("Retrieving track information for %i", track_num);

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* at this point we know the type of the disc that's why we set the 
	 * size according to this type. That may help to avoid outrange address
	 * errors. */
	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DUAL_L|BRASERO_MEDIUM_WRITABLE))
		size = 48;
	else if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_PLUS|BRASERO_MEDIUM_WRITABLE))
		size = 40;
	else
		size = 36;

	result = brasero_mmc1_read_track_info (handle,
					       track_num,
					       &track_info,
					       &size,
					       code);

	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("READ TRACK INFO failed");
		return BRASERO_BURN_ERR;
	}

	track->blocks_num = BRASERO_GET_32 (track_info.track_size);
	track->session = BRASERO_SCSI_SESSION_NUM (track_info);

	if (track->blocks_num <= 300) {
		/* Now here is a potential bug: we can write tracks (data or
		 * not) shorter than 300 KiB /2 sec but they will be padded to
		 * reach this floor value. It means that blocks_num is always
		 * 300 blocks even if the data length on the track is actually
		 * shorter.
		 * So we read the volume descriptor. If it works, good otherwise
		 * use the old value.
		 * That's important for checksuming to have a perfect account of
		 * the data size. */
		BRASERO_MEDIA_LOG ("300 sectors size. Checking for real size");
		brasero_medium_track_volume_size (self, track, handle);
	}
	/* NOTE: for multisession CDs only
	 * if the session was incremental (TAO/packet/...) by opposition to DAO
	 * and SAO, then 2 blocks (run-out) have been added at the end of user
	 * track for linking. That's why we have 2 additional sectors when the
	 * track has been recorded in TAO mode
	 * See MMC5
	 * 6.44.3.2 CD-R Fixed Packet, Variable Packet, Track-At-Once
	 * Now, strangely track_get_info always removes two blocks, whereas read
	 * raw toc adds them (always) and this, whatever the mode, the position.
	 * It means that when we detect a SAO session we have to add 2 blocks to
	 * all tracks in it. 
	 * See # for any information:
	 * if first track is recorded in SAO/DAO then the length will be two sec
	 * shorter. If not, if it was recorded in TAO, that's fine.
	 * The other way would be to use read raw toc but then that's the
	 * opposite that happens and that latter will return two more bytes for
	 * TAO recorded session.
	 * So there are 2 workarounds:
	 * - read the volume size (can be unreliable)
	 * - read the 2 last blocks and see if they are run-outs
	 * here we do solution 2 but only for CDRW, not blank, and for first
	 * session only since that's the only one that can be recorded in DAO. */
	else if (track->session == 1
	     && (track->type & BRASERO_MEDIUM_TRACK_DATA)
	     &&  multisession
	     &&  (priv->info & BRASERO_MEDIUM_CD)
	     && !(priv->info & BRASERO_MEDIUM_ROM)) {
		BRASERO_MEDIA_LOG ("Data track belongs to first session of multisession CD. "
				  "Checking for real size (%i sectors currently).",
				  track->blocks_num);

		/* we test the pregaps blocks for TDB: these are special blocks
		 * filling the pregap of a track when it was recorded as TAO or
		 * as Packet.
		 * NOTE: in this case we need to skip 7 sectors before since if
		 * it was recorded incrementally then there is also 4 runins,
		 * 1 link sector and 2 runouts (at end of pregap). 
		 * we also make sure that the two blocks we're adding are
		 * actually readable. */
		/* Test the last block, the before last and the one before before last */
		result = brasero_mmc1_read_block (handle,
						  FALSE,
						  BRASERO_SCSI_BLOCK_TYPE_ANY,
						  BRASERO_SCSI_BLOCK_HEADER_NONE,
						  BRASERO_SCSI_BLOCK_NO_SUBCHANNEL,
						  track->blocks_num + track->start,
						  2,
						  NULL,
						  0,
						  NULL);

		if (result == BRASERO_SCSI_OK) {
			BRASERO_MEDIA_LOG ("Following two sectors are readable.");

			if (brasero_medium_track_written_SAO (handle, track_num, track->start)) {
				track->blocks_num += 2;
				BRASERO_MEDIA_LOG ("Correcting track size (now %i)", track->blocks_num);
			}
		}
		else
			BRASERO_MEDIA_LOG ("Detected runouts");
	}

	/* NOTE: DVD+RW, DVD-RW (restricted overwrite) never reach this function */
	BRASERO_MEDIA_LOG ("Track %i (session %i): type = %i start = %llu size = %llu",
			  track_num,
			  track->session,
			  track->type,
			  track->start,
			  track->blocks_num);

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_track_set_leadout_DVDR_blank (BraseroMedium *self,
					     BraseroDeviceHandle *handle,
					     BraseroMediumTrack *leadout,
					     BraseroScsiErrCode *code)
{
	BraseroScsiFormatCapacitiesHdr *hdr = NULL;
	BraseroScsiMaxCapacityDesc *current;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	BRASERO_MEDIA_LOG ("Using fallback method for blank CDR to retrieve NWA and leadout information");

	/* NWA is easy for blank DVD-Rs, it's 0. So far, so good... */
	priv->next_wr_add = 0;

	result = brasero_mmc2_read_format_capacities (handle,
						      &hdr,
						      &size,
						      code);
	if (result != BRASERO_SCSI_OK) {
		g_free (hdr);

		BRASERO_MEDIA_LOG ("READ FORMAT CAPACITIES failed");
		return BRASERO_BURN_ERR;
	}

	/* See if the media is already formatted which means for -R media that 
	 * they are blank. */
	current = hdr->max_caps;
	if (current->type & BRASERO_SCSI_DESC_FORMATTED) {
		BRASERO_MEDIA_LOG ("Unformatted medium");
		g_free (hdr);
		return BRASERO_BURN_ERR;
	}
		
	BRASERO_MEDIA_LOG ("Unformatted medium");

	/* of course it starts at 0 since it's empty */
	leadout->start = 0;
	leadout->blocks_num = BRASERO_GET_32 (current->blocks_num);

	BRASERO_MEDIA_LOG ("Leadout (through READ FORMAT CAPACITIES): start = %llu size = %llu",
			  leadout->start,
			  leadout->blocks_num);

	g_free (hdr);
	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_track_set_leadout_CDR_blank (BraseroMedium *self,
					    BraseroDeviceHandle *handle,
					    BraseroMediumTrack *leadout,
					    BraseroScsiErrCode *code)
{
	BraseroScsiAtipData *atip = NULL;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size = 0;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	BRASERO_MEDIA_LOG ("Using fallback method for blank CDR to retrieve NWA and leadout information");

	/* NWA is easy for blank CDRs, it's 0. So far, so good... */
	priv->next_wr_add = 0;

	result = brasero_mmc1_read_atip (handle, &atip, &size, code);
	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("READ ATIP failed");
		return BRASERO_BURN_ERR;
	}

	leadout->blocks_num = atip->desc->leadout_mn * 60 * 75 +
			      atip->desc->leadout_sec * 75 +
			      atip->desc->leadout_frame;

	/* of course it starts at 0 since it's empty */
	leadout->start = 0;

	BRASERO_MEDIA_LOG ("Leadout (through READ ATIP): start = %llu size = %llu",
			  leadout->start,
			  leadout->blocks_num);

	g_free (atip);

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_set_write_mode_page (BraseroMedium *self,
				    BraseroDeviceHandle *handle,
				    BraseroScsiErrCode *code)
{
	BraseroScsiModeData *data = NULL;
	BraseroScsiWritePage *wrt_page;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size;

	BRASERO_MEDIA_LOG ("Setting write mode page");

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* NOTE: this works for CDR, DVDR+-, BDR-SRM */
	/* make sure the current write mode is TAO. Otherwise the drive will
	 * return the first sector of the pregap instead of the first user
	 * accessible sector. */
	result = brasero_spc1_mode_sense_get_page (handle,
						   BRASERO_SPC_PAGE_WRITE,
						   &data,
						   &size,
						   code);
	if (result == BRASERO_SCSI_OK) {
		wrt_page = (BraseroScsiWritePage *) &data->page;

		BRASERO_MEDIA_LOG ("Former write type %d", wrt_page->write_type);
		BRASERO_MEDIA_LOG ("Former track mode %d", wrt_page->track_mode);
		BRASERO_MEDIA_LOG ("Former data block type %d", wrt_page->data_block_type);

		/* "reset some stuff to be on the safe side" (words and ideas
		 * taken from k3b:)). */
		wrt_page->ps = 0;
		wrt_page->BUFE = 0;
		wrt_page->multisession = 0;
		wrt_page->testwrite = 0;
		wrt_page->LS_V = 0;
		wrt_page->copy = 0;
		wrt_page->FP = 0;
		wrt_page->session_format = 0;
		BRASERO_SET_16 (wrt_page->pause_len, 150);

		if (priv->info & BRASERO_MEDIUM_CD) {
			wrt_page->write_type = BRASERO_SCSI_WRITE_TAO;
			wrt_page->track_mode = 4;
		}
		else if (priv->info & BRASERO_MEDIUM_DVD) {
			wrt_page->write_type = BRASERO_SCSI_WRITE_PACKET_INC;
			wrt_page->track_mode = 5;
		}

		wrt_page->data_block_type = 8;

		result = brasero_spc1_mode_select (handle, data, size, code);
		g_free (data);

		if (result != BRASERO_SCSI_OK) {
			BRASERO_MEDIA_LOG ("MODE SELECT failed");

			/* This isn't necessarily a problem! we better try */
			return BRASERO_BURN_ERR;
		}
	}
	else {
		BRASERO_MEDIA_LOG ("MODE SENSE failed");
		/* This isn't necessarily a problem! we better try the rest */
		return BRASERO_BURN_ERR;
	}

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_track_set_leadout (BraseroMedium *self,
				  BraseroDeviceHandle *handle,
				  BraseroMediumTrack *leadout,
				  BraseroScsiErrCode *code)
{
	BraseroScsiTrackInfo track_info;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	gint track_num;
	int size;

	BRASERO_MEDIA_LOG ("Retrieving NWA and leadout information");

	priv = BRASERO_MEDIUM_PRIVATE (self);

	if (BRASERO_MEDIUM_RANDOM_WRITABLE (priv->info)) {
		BRASERO_MEDIA_LOG ("Overwritable medium  => skipping");
		return BRASERO_BURN_OK;
	}

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDR)) {
		/* This is necessary to make sure nwa won't be the start of the
		 * pregap if the current write mode is SAO with blank CDR.
		 * Carry on even if it fails.
		 * This can work with CD-R/W and DVD-R/W. + media don't use the
		 * write mode page anyway. */
		result = brasero_medium_set_write_mode_page (self, handle, code);
		if (result == BRASERO_BURN_ERR
		&&  BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDR|BRASERO_MEDIUM_BLANK))
			return brasero_medium_track_set_leadout_CDR_blank (self,
									   handle,
									   leadout,
									   code);
	}

	/* At this point we know the type of the disc that's why we set the 
	 * size according to this type. That may help to avoid outrange address
	 * errors. */
	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DUAL_L|BRASERO_MEDIUM_WRITABLE))
		size = 48;
	else if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_PLUS|BRASERO_MEDIUM_WRITABLE))
		size = 40;
	else
		size = 36;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDR)
	||  BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDRW)
	/* The following includes DL */
	||  BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDR_PLUS)) 
		track_num = 0xFF;
	else if (priv->first_open_track >= 0)
		track_num = priv->first_open_track;
	else {
		BRASERO_MEDIA_LOG ("There aren't any open session set");
		return BRASERO_BURN_ERR;
	}

	result = brasero_mmc1_read_track_info (handle,
					       track_num,
					       &track_info,
					       &size,
					       code);
	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("READ TRACK INFO failed");

		/* This only for CD-R */
		if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDR|BRASERO_MEDIUM_BLANK))
			return brasero_medium_track_set_leadout_CDR_blank (self,
									   handle,
									   leadout,
									   code);
		else if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BLANK))
			return brasero_medium_track_set_leadout_DVDR_blank (self,
									    handle,
									    leadout,
									    code);
			 
		return BRASERO_BURN_ERR;
	}

	BRASERO_MEDIA_LOG ("Next Writable Address is %d", BRASERO_GET_32 (track_info.next_wrt_address));
	if (track_info.next_wrt_address_valid)
		priv->next_wr_add = BRASERO_GET_32 (track_info.next_wrt_address);
	else
		BRASERO_MEDIA_LOG ("Next Writable Address is not valid");

	/* Set free space */
	BRASERO_MEDIA_LOG ("Free blocks %d", BRASERO_GET_32 (track_info.free_blocks));
	leadout->blocks_num = BRASERO_GET_32 (track_info.free_blocks);

	if (!leadout->blocks_num) {
		leadout->blocks_num = BRASERO_GET_32 (track_info.track_size);
		BRASERO_MEDIA_LOG ("Using track size %d", leadout->blocks_num);
	}

	if (!leadout->blocks_num
	&&   BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BLANK))
		return brasero_medium_track_set_leadout_DVDR_blank (self,
								    handle,
								    leadout,
								    code);

	BRASERO_MEDIA_LOG ("Leadout: start = %llu size = %llu",
			  leadout->start,
			  leadout->blocks_num);

	return BRASERO_BURN_OK;
}

/**
 * NOTE: for DVD-R multisession we lose 28688 blocks for each session
 * so the capacity is the addition of all session sizes + 28688 for each
 * For all multisession DVD-/+R and CDR-RW the remaining size is given 
 * in the leadout. One exception though with DVD+/-RW.
 */

static void
brasero_medium_add_DVD_plus_RW_leadout (BraseroMedium *self)
{
	BraseroMediumTrack *leadout;
	BraseroMediumPrivate *priv;
	gint64 blocks_num;
	gint32 start;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* determine the start */
	if (priv->tracks) {
		BraseroMediumTrack *track;

		track = priv->tracks->data;
		start = track->start + track->blocks_num;
		blocks_num = priv->block_num - ((track->blocks_num > 300) ? track->blocks_num : 300);
	}
	else {
		start = 0;
		blocks_num = priv->block_num;
	}

	leadout = g_new0 (BraseroMediumTrack, 1);
	priv->tracks = g_slist_append (priv->tracks, leadout);

	leadout->start = start;
	leadout->blocks_num = blocks_num;
	leadout->type = BRASERO_MEDIUM_TRACK_LEADOUT;

	/* we fabricate the leadout here. We don't really need one in 
	 * fact since it is always at the last sector whatever the
	 * amount of data written. So we need in fact to read the file
	 * system and get the last sector from it. Hopefully it won't be
	 * buggy */
	priv->next_wr_add = 0;

	BRASERO_MEDIA_LOG ("Adding fabricated leadout start = %llu length = %llu",
			  leadout->start,
			  leadout->blocks_num);
}

static BraseroBurnResult
brasero_medium_get_sessions_info (BraseroMedium *self,
				  BraseroDeviceHandle *handle,
				  BraseroScsiErrCode *code)
{
	int num, i, size;
	gboolean multisession;
	BraseroScsiResult result;
	BraseroScsiTocDesc *desc;
	BraseroMediumPrivate *priv;
	BraseroScsiFormattedTocData *toc = NULL;

	BRASERO_MEDIA_LOG ("Reading Toc");

	priv = BRASERO_MEDIUM_PRIVATE (self);
	result = brasero_mmc1_read_toc_formatted (handle,
						  0,
						  &toc,
						  &size,
						  code);
	if (result != BRASERO_SCSI_OK) {
		g_free (toc);

		BRASERO_MEDIA_LOG ("READ TOC failed");
		return BRASERO_BURN_ERR;
	}

	num = (size - sizeof (BraseroScsiFormattedTocData)) /
	       sizeof (BraseroScsiTocDesc);

	/* remove 1 for leadout */
	multisession = !(priv->info & BRASERO_MEDIUM_BLANK) && num > 0;

	/* NOTE: in the case of DVD- there are always only 3 sessions if they
	 * are open: all first concatenated sessions, the last session, and the
	 * leadout. */
	
	BRASERO_MEDIA_LOG ("%i track(s) found", num);

	desc = toc->desc;
	for (i = 0; i < num; i ++, desc ++) {
		BraseroMediumTrack *track;

		if (desc->track_num == BRASERO_SCSI_TRACK_LEADOUT_START) {
			BRASERO_MEDIA_LOG ("Leadout reached %d",
					  BRASERO_GET_32 (desc->track_start));
			break;
		}

		track = g_new0 (BraseroMediumTrack, 1);
		priv->tracks = g_slist_prepend (priv->tracks, track);
		track->start = BRASERO_GET_32 (desc->track_start);

		/* we shouldn't request info on a track if the disc is closed */
		if (desc->control & BRASERO_SCSI_TRACK_COPY)
			track->type |= BRASERO_MEDIUM_TRACK_COPY;

		if (!(desc->control & BRASERO_SCSI_TRACK_DATA)) {
			track->type |= BRASERO_MEDIUM_TRACK_AUDIO;
			priv->info |= BRASERO_MEDIUM_HAS_AUDIO;

			if (desc->control & BRASERO_SCSI_TRACK_PREEMP)
				track->type |= BRASERO_MEDIUM_TRACK_PREEMP;

			if (desc->control & BRASERO_SCSI_TRACK_4_CHANNELS)
				track->type |= BRASERO_MEDIUM_TRACK_4_CHANNELS;
		}
		else {
			track->type |= BRASERO_MEDIUM_TRACK_DATA;
			priv->info |= BRASERO_MEDIUM_HAS_DATA;

			if (desc->control & BRASERO_SCSI_TRACK_DATA_INCREMENTAL)
				track->type |= BRASERO_MEDIUM_TRACK_INCREMENTAL;
		}

		if (BRASERO_MEDIUM_RANDOM_WRITABLE (priv->info)) {
			BraseroBurnResult result;

			/* A special case for these kinds of media (DVD+RW, ...)
			 * which have only one track: the first. Since it's not
			 * possible to know the amount of data that were really
			 * written in this session, read the filesystem. */
			BRASERO_MEDIA_LOG ("DVD+RW (DL) or DVD-RW (restricted overwrite) checking volume size (start = %i)", track->start);
			track->session = 1;
			track->start = 0;
			result = brasero_medium_track_volume_size (self, 
								   track,
								   handle);
			if (result != BRASERO_BURN_OK) {
				priv->tracks = g_slist_remove (priv->tracks, track);
				g_free (track);

				priv->info |= BRASERO_MEDIUM_BLANK;
				priv->info &= ~(BRASERO_MEDIUM_CLOSED|
					        BRASERO_MEDIUM_HAS_DATA);

				BRASERO_MEDIA_LOG ("Empty first session.");
			}
			else {
				priv->next_wr_add = 0;
				BRASERO_MEDIA_LOG ("Track 1 (session %i): type = %i start = %llu size = %llu",
						  track->session,
						  track->type,
						  track->start,
						  track->blocks_num);
			}

			/* NOTE: the next track should be the leadout */
			continue;
		}

		if (priv->probe_cancelled) {
			g_free (toc);
			return BRASERO_BURN_CANCEL;
		}

		brasero_medium_track_get_info (self,
					       multisession,
					       track,
					       g_slist_length (priv->tracks),
					       handle,
					       code);
	}

	if (priv->probe_cancelled) {
		g_free (toc);
		return BRASERO_BURN_CANCEL;
	}

	/* put the tracks in the right order */
	priv->tracks = g_slist_reverse (priv->tracks);

	if (BRASERO_MEDIUM_RANDOM_WRITABLE (priv->info))
		brasero_medium_add_DVD_plus_RW_leadout (self);
	else if (!(priv->info & BRASERO_MEDIUM_CLOSED)) {
		BraseroMediumTrack *leadout;

		/* we shouldn't request info on leadout if the disc is closed
		 * (except for DVD+/- (restricted) RW (see above) */
		leadout = g_new0 (BraseroMediumTrack, 1);
		leadout->start = BRASERO_GET_32 (desc->track_start);
		leadout->type = BRASERO_MEDIUM_TRACK_LEADOUT;
		priv->tracks = g_slist_append (priv->tracks, leadout);

		brasero_medium_track_set_leadout (self,
						  handle,
						  leadout,
						  code);
	}

	g_free (toc);

	return BRASERO_BURN_OK;
}

static void
brasero_medium_get_DVD_id (BraseroMedium *self,
			   BraseroDeviceHandle *handle,
			   BraseroScsiErrCode *code)
{
	gint size = 0;
	BraseroScsiResult result;
	BraseroMediumPrivate *priv;
	BraseroScsiReadDiscStructureHdr *hdr = NULL;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* This should be only possible for DVD-R(W) and not with all drives */
	result = brasero_mmc2_read_generic_structure (handle,
						      BRASERO_SCSI_FORMAT_LESS_MEDIA_ID_DVD,
						      &hdr,
						      &size,
						      code);
	if (result != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("Retrieval of DVD id failed");
		return;
	}

	BRASERO_MEDIA_LOG ("DVD id %d", BRASERO_GET_16 (hdr->data + 2));
	priv->id = g_strdup_printf ("%d", BRASERO_GET_16 (hdr->data + 2));
	g_free (hdr);
}

static BraseroBurnResult
brasero_medium_set_blank (BraseroMedium *self,
			  BraseroDeviceHandle *handle,
			  gint first_open_track,
			  BraseroScsiErrCode *code)
{
	BraseroMediumPrivate *priv;
	BraseroMediumTrack *track;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	BRASERO_MEDIA_LOG ("Empty media");

	priv->info |= BRASERO_MEDIUM_BLANK;
	priv->block_size = 2048;

	priv->first_open_track = first_open_track;
	BRASERO_MEDIA_LOG ("First open track %d", priv->first_open_track);

	if (BRASERO_MEDIUM_RANDOM_WRITABLE (priv->info))
		brasero_medium_add_DVD_plus_RW_leadout (self);
	else {
		track = g_new0 (BraseroMediumTrack, 1);
		track->start = 0;
		track->type = BRASERO_MEDIUM_TRACK_LEADOUT;
		priv->tracks = g_slist_prepend (priv->tracks, track);
			
		brasero_medium_track_set_leadout (self,
						  handle,
						  track,
						  code);
	}

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_get_contents (BraseroMedium *self,
			     BraseroDeviceHandle *handle,
			     BraseroScsiErrCode *code)
{
	int size;
	BraseroBurnResult res;
	BraseroScsiResult result;
	BraseroMediumPrivate *priv;
	BraseroScsiDiscInfoStd *info = NULL;

	BRASERO_MEDIA_LOG ("Retrieving media status");

	priv = BRASERO_MEDIUM_PRIVATE (self);

	result = brasero_mmc1_read_disc_information_std (handle,
							 &info,
							 &size,
							 code);
	if (result != BRASERO_SCSI_OK) {
		g_free (info);
	
		BRASERO_MEDIA_LOG ("READ DISC INFORMATION failed");
		return BRASERO_BURN_ERR;
	}

	if (info->disc_id_valid) {
		/* Try to get the disc identification if possible (CDs only) */
		BRASERO_MEDIA_LOG ("Disc id %i", BRASERO_GET_32 (info->disc_id));
		priv->id = g_strdup_printf ("%d", BRASERO_GET_32 (info->disc_id));
	}
	else if (priv->info & BRASERO_MEDIUM_DVD)
		brasero_medium_get_DVD_id (self, handle, code);

	if (info->erasable)
		priv->info |= BRASERO_MEDIUM_REWRITABLE;

	priv->first_open_track = -1;

	if (info->status == BRASERO_SCSI_DISC_EMPTY) {
		res = brasero_medium_set_blank (self,
						handle,
						BRASERO_FIRST_TRACK_IN_LAST_SESSION (info),
						code);
	}
	else if (info->status == BRASERO_SCSI_DISC_INCOMPLETE) {
		if (!BRASERO_MEDIUM_RANDOM_WRITABLE (priv->info)) {
			priv->info |= BRASERO_MEDIUM_APPENDABLE;
			BRASERO_MEDIA_LOG ("Appendable media");

			priv->first_open_track = BRASERO_FIRST_TRACK_IN_LAST_SESSION (info);
			BRASERO_MEDIA_LOG ("First track in last open session %d", priv->first_open_track);

			res = brasero_medium_get_sessions_info (self, handle, code);
		}
		else {
			/* if that type of media is in incomplete state that
			 * means it has just been formatted. And therefore it's
			 * blank. */
			res = brasero_medium_set_blank (self,
							handle,
							BRASERO_FIRST_TRACK_IN_LAST_SESSION (info),
							code);
		}
	}
	else if (info->status == BRASERO_SCSI_DISC_FINALIZED) {
		priv->info |= BRASERO_MEDIUM_CLOSED;
		BRASERO_MEDIA_LOG ("Closed media");

		res = brasero_medium_get_sessions_info (self, handle, code);
	}

	g_free (info);
	return res;
}

/**
 * Some identification functions
 */

static BraseroBurnResult
brasero_medium_get_medium_type (BraseroMedium *self,
				BraseroDeviceHandle *handle,
				BraseroScsiErrCode *code)
{
	BraseroScsiProfile profile;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;

	BRASERO_MEDIA_LOG ("Retrieving media profile");

	priv = BRASERO_MEDIUM_PRIVATE (self);
	result = brasero_mmc2_get_profile (handle, &profile, code);

	if (result != BRASERO_SCSI_OK) {
		BraseroScsiAtipData *data = NULL;
		int size = 0;

		BRASERO_MEDIA_LOG ("GET CONFIGURATION failed");

		/* This could be a MMC1 drive since this command was
		 * introduced in MMC2 and is supported onward. So it
		 * has to be a CD (R/RW). The rest of the information
		 * will be provided by read_disc_information. */

		/* retrieve the speed */
		result = brasero_medium_get_page_2A_write_speed_desc (self,
								      handle,
								      code);

		/* If this fails it means that this drive is probably older than
		 * MMC1 spec or does not conform to it. */
		if (result != BRASERO_BURN_OK)
			return BRASERO_BURN_ERR;

		/* The only thing here left to determine is if that's a WRITABLE
		 * or a REWRITABLE. To determine that information, we need to
		 * read TocPmaAtip. It if fails that's a ROM, if it succeeds.
		 * No need to set error code since we consider that it's a ROM
		 * if a failure happens. */
		result = brasero_mmc1_read_atip (handle,
						 &data,
						 &size,
						 NULL);
		if (result != BRASERO_SCSI_OK) {
			/* CDROM */
			priv->info = BRASERO_MEDIUM_CDROM;
			priv->type = types [1];
		}
		else {
			/* check the size of the structure: it must be at least 8 bytes long */
			if (size < 8) {
				if (size)
					g_free (data);

				BRASERO_MEDIA_LOG ("READ ATIP failed (wrong size)");
				return BRASERO_BURN_ERR;
			}

			if (data->desc->erasable) {
				/* CDRW */
				priv->info = BRASERO_MEDIUM_CDRW;
				priv->type = types [3];
			}
			else {
				/* CDR */
				priv->info = BRASERO_MEDIUM_CDR;
				priv->type = types [2];
			}

			g_free (data);
		}

		return result;
	}

	switch (profile) {
	case BRASERO_SCSI_PROF_CDROM:
		priv->info = BRASERO_MEDIUM_CDROM;
		priv->type = types [1];
		break;

	case BRASERO_SCSI_PROF_CDR:
		priv->info = BRASERO_MEDIUM_CDR;
		priv->type = types [2];
		break;

	case BRASERO_SCSI_PROF_CDRW:
		priv->info = BRASERO_MEDIUM_CDRW;
		priv->type = types [3];
		break;

	case BRASERO_SCSI_PROF_DVD_ROM:
		priv->info = BRASERO_MEDIUM_DVD_ROM;
		priv->type = types [4];
		break;

	case BRASERO_SCSI_PROF_DVD_R:
		priv->info = BRASERO_MEDIUM_DVDR;
		priv->type = types [5];
		break;

	case BRASERO_SCSI_PROF_DVD_RW_RESTRICTED:
		priv->info = BRASERO_MEDIUM_DVDRW_RESTRICTED;
		priv->type = types [6];
		break;

	case BRASERO_SCSI_PROF_DVD_RW_SEQUENTIAL:
		priv->info = BRASERO_MEDIUM_DVDRW;
		priv->type = types [6];
		break;

	case BRASERO_SCSI_PROF_DVD_R_PLUS:
		priv->info = BRASERO_MEDIUM_DVDR_PLUS;
		priv->type = types [7];
		break;

	case BRASERO_SCSI_PROF_DVD_RW_PLUS:
		priv->info = BRASERO_MEDIUM_DVDRW_PLUS;
		priv->type = types [8];
		break;

	case BRASERO_SCSI_PROF_DVD_R_PLUS_DL:
		priv->info = BRASERO_MEDIUM_DVDR_PLUS_DL;
		priv->type = types [9];
		break;

	case BRASERO_SCSI_PROF_DVD_RW_PLUS_DL:
		priv->info = BRASERO_MEDIUM_DVDRW_PLUS_DL;
		priv->type = types [10];
		break;

	case BRASERO_SCSI_PROF_DVD_R_DL_SEQUENTIAL:
		priv->info = BRASERO_MEDIUM_DVDR_DL;
		priv->type = types [11];
		break;

	case BRASERO_SCSI_PROF_DVD_R_DL_JUMP:
		priv->info = BRASERO_MEDIUM_DVDR_JUMP_DL;
		priv->type = types [11];
		break;

	case BRASERO_SCSI_PROF_BD_ROM:
		priv->info = BRASERO_MEDIUM_BD_ROM;
		priv->type = types [13];
		break;

	case BRASERO_SCSI_PROF_BR_R_SEQUENTIAL:
		/* check if that's a POW later */
		priv->info = BRASERO_MEDIUM_BDR_SRM;
		priv->type = types [14];
		break;

	case BRASERO_SCSI_PROF_BR_R_RANDOM:
		priv->info = BRASERO_MEDIUM_BDR_RANDOM;
		priv->type = types [14];
		break;

	case BRASERO_SCSI_PROF_BD_RW:
		priv->info = BRASERO_MEDIUM_BDRE;
		priv->type = types [15];
		break;

	case BRASERO_SCSI_PROF_DVD_RAM:
		priv->info = BRASERO_MEDIUM_DVD_RAM;
		priv->type = types [12];
		break;

	/* WARNING: these types are recognized, no more */
	case BRASERO_SCSI_PROF_NON_REMOVABLE:
	case BRASERO_SCSI_PROF_REMOVABLE:
	case BRASERO_SCSI_PROF_MO_ERASABLE:
	case BRASERO_SCSI_PROF_MO_WRITE_ONCE:
	case BRASERO_SCSI_PROF_MO_ADVANCED_STORAGE:
	case BRASERO_SCSI_PROF_DDCD_ROM:
	case BRASERO_SCSI_PROF_DDCD_R:
	case BRASERO_SCSI_PROF_DDCD_RW:
	case BRASERO_SCSI_PROF_HD_DVD_ROM:
	case BRASERO_SCSI_PROF_HD_DVD_R:
	case BRASERO_SCSI_PROF_HD_DVD_RAM:
		priv->info = BRASERO_MEDIUM_UNSUPPORTED;
		return BRASERO_BURN_NOT_SUPPORTED;
	}

	/* Get a more precise idea of what sequential BD-R type we have here */
	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BDR_SRM)) {
		BraseroScsiGetConfigHdr *hdr = NULL;
		int size = 0;

		/* check for POW type */
		result = brasero_mmc2_get_configuration_feature (handle,
								 BRASERO_SCSI_FEAT_BDR_POW,
								 &hdr,
								 &size,
								 code);
		if (result == BRASERO_SCSI_OK) {
			if (hdr->desc->current) {
				BRASERO_MEDIA_LOG ("POW formatted medium detected");
				priv->info |= BRASERO_MEDIUM_POW;
			}

			g_free (hdr);			
		}
		else {
			BraseroScsiFormatCapacitiesHdr *hdr = NULL;

			/* NOTE: the disc status as far as format is concerned
			 * is done later for all rewritable media. */
			/* check for unformatted media (if it's POW or RANDOM
			 * there is no need of course) */
			result = brasero_mmc2_read_format_capacities (handle,
								      &hdr,
								      &size,
								      NULL);
			if (result == BRASERO_SCSI_OK) {
				BraseroScsiMaxCapacityDesc *current;

				current = hdr->max_caps;
				if (!(current->type & BRASERO_SCSI_DESC_FORMATTED)) {
					BRASERO_MEDIA_LOG ("Unformatted BD-R");
					priv->info |= BRASERO_MEDIUM_UNFORMATTED;
				}

				g_free (hdr);
			}
		}		
	}

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BD)) {
		/* FIXME: check for dual layer BD */
	}

	return BRASERO_BURN_OK;
}

static BraseroBurnResult
brasero_medium_get_css_feature (BraseroMedium *self,
				BraseroDeviceHandle *handle,
				BraseroScsiErrCode *code)
{
	BraseroScsiGetConfigHdr *hdr = NULL;
	BraseroMediumPrivate *priv;
	BraseroScsiResult result;
	int size;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	BRASERO_MEDIA_LOG ("Testing for Css encrypted media");
	result = brasero_mmc2_get_configuration_feature (handle,
							 BRASERO_SCSI_FEAT_DVD_CSS,
							 &hdr,
							 &size,
							 code);
	if (result != BRASERO_SCSI_OK) {
		g_free (hdr);

		BRASERO_MEDIA_LOG ("GET CONFIGURATION failed");
		return BRASERO_BURN_ERR;
	}

	if (hdr->desc->add_len < sizeof (BraseroScsiDVDCssDesc)) {
		g_free (hdr);
		return BRASERO_BURN_OK;
	}

	/* here we just need to see if this feature is current or not */
	if (hdr->desc->current) {
		priv->info |= BRASERO_MEDIUM_PROTECTED;
		BRASERO_MEDIA_LOG ("media is Css protected");
	}

	g_free (hdr);
	return BRASERO_BURN_OK;
}

static gboolean
brasero_medium_get_CD_TEXT (BraseroMedium *medium,
			    int type,
			    int track_num,
			    guint charset_CD_TEXT,
			    gboolean double_byte,
			    const char *string)
{
	char *utf8_string;
	BraseroMediumPrivate *priv;
	const gchar *charset = NULL;

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	/* For the moment we're only interested in medium title but that could
	 * be extented to all tracks information. */
	switch (type) {
	case BRASERO_SCSI_CD_TEXT_ALBUM_TITLE:
		if (track_num)
			return FALSE;

		break;

	case BRASERO_SCSI_CD_TEXT_PERFORMER_NAME:
	case BRASERO_SCSI_CD_TEXT_SONGWRITER_NAME:
	case BRASERO_SCSI_CD_TEXT_COMPOSER_NAME:
	case BRASERO_SCSI_CD_TEXT_ARRANGER_NAME:
	case BRASERO_SCSI_CD_TEXT_ARTIST_NAME:
	case BRASERO_SCSI_CD_TEXT_DISC_ID_INFO:
	case BRASERO_SCSI_CD_TEXT_GENRE_ID_INFO:
	case BRASERO_SCSI_CD_TEXT_UPC_EAN_ISRC:
	default:
		return FALSE;
	}

	g_get_charset (&charset);

	/* It's ASCII so convert to locale */
	switch (charset_CD_TEXT) {
	case BRASERO_CD_TEXT_8859_1:
		utf8_string = g_convert_with_fallback (string,
						       -1,
						       charset,
						       "ISO-8859-1",
						       "_",
						       NULL,
						       NULL,
						       NULL);
		break;
	case BRASERO_CD_TEXT_KANJI:
		utf8_string = g_convert_with_fallback (string,
						       -1,
						       charset,
						       "EUC-JP",
						       "_",
						       NULL,
						       NULL,
						       NULL);
		break;
	case BRASERO_CD_TEXT_KOREAN:
		utf8_string = g_convert_with_fallback (string,
						       -1,
						       charset,
						       "EUC-KR",
						       "_",
						       NULL,
						       NULL,
						       NULL);
		break;
	case BRASERO_CD_TEXT_CHINESE:
		utf8_string = g_convert_with_fallback (string,
						       -1,
						       charset,
						       "GB2312",
						       "_",
						       NULL,
						       NULL,
						       NULL);
		break;
	default:
	case BRASERO_CD_TEXT_ASCII:
		utf8_string = g_convert_with_fallback (string,
						       -1,
						       charset,
						       "ASCII",
						       "_",
						       NULL,
						       NULL,
						       NULL);
	}


	if (priv->CD_TEXT_title)
		g_free (priv->CD_TEXT_title);

	if (!utf8_string) {
		BRASERO_MEDIA_LOG ("Charset convertion failed");
		priv->CD_TEXT_title = g_strdup (string);
	}
	else
		priv->CD_TEXT_title = utf8_string;

	BRASERO_MEDIA_LOG ("CD-TEXT title %s", priv->CD_TEXT_title);
	return TRUE;
}

static int
_next_CD_TEXT_pack (BraseroScsiCDTextData *cd_text,
		    int current,
		    int max)
{
	current ++;
	if (current >= max)
		return -1;

	/* Skip all packs we're not interested or are not valid */
	while (cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_ALBUM_TITLE &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_PERFORMER_NAME &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_SONGWRITER_NAME &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_COMPOSER_NAME &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_ARRANGER_NAME &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_ARTIST_NAME &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_DISC_ID_INFO &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_GENRE_ID_INFO &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_UPC_EAN_ISRC &&
	       cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_BLOCK_SIZE) {
		current ++;
		if (current > max)
			return -1;
	}

	return current;
}

static gboolean
brasero_medium_read_CD_TEXT_block_info (BraseroScsiCDTextData *cd_text,
					int current,
					int max,
					gchar *buffer)
{
	while ((current = _next_CD_TEXT_pack (cd_text, current, max)) != -1) {
		off_t offset = 0;

		if (cd_text->pack [current].type != BRASERO_SCSI_CD_TEXT_BLOCK_SIZE)
			continue;

		do {
			memcpy (buffer + offset,
				cd_text->pack [current].text,
				sizeof (cd_text->pack [current].text));

			offset += sizeof (cd_text->pack [current].text);
			current = _next_CD_TEXT_pack (cd_text, current, max);
		} while (current != -1 && cd_text->pack [current].type == BRASERO_SCSI_CD_TEXT_BLOCK_SIZE);

		return TRUE;
	}

	return FALSE;
}

static void
brasero_medium_read_CD_TEXT (BraseroMedium *self,
			     BraseroDeviceHandle *handle,
			     BraseroScsiErrCode *code)
{
	int off;
	gint charset;
	int track_num;
	int num, size, i;
	char buffer [256]; /* mmc specs advise no more than 160 */
	gboolean find_block_info;
	BraseroMediumPrivate *priv;
	BraseroScsiCDTextData *cd_text;

	BRASERO_MEDIA_LOG ("Getting CD-TEXT");
	if (brasero_mmc3_read_cd_text (handle, &cd_text, &size, code) != BRASERO_SCSI_OK) {
		BRASERO_MEDIA_LOG ("GET CD-TEXT failed");
		return;
	}

	/* Get the number of CD-Text Data Packs */
	num = (BRASERO_GET_16 (cd_text->hdr->len) -
	      (sizeof (BraseroScsiTocPmaAtipHdr) - sizeof (cd_text->hdr->len)))  /
	       sizeof (BraseroScsiCDTextPackData);

	priv = BRASERO_MEDIUM_PRIVATE (self);

	off = 0;
	track_num = 0;
	charset = BRASERO_CD_TEXT_ASCII;

	i = -1;
	find_block_info = TRUE;
	while ((i = _next_CD_TEXT_pack (cd_text, i, num)) != -1) {
		int j;
		gboolean is_double_byte;

		/* skip these until the start of another language block or the end */
		if (cd_text->pack [i].type == BRASERO_SCSI_CD_TEXT_BLOCK_SIZE) {
			find_block_info = TRUE;
			continue;
		}

		if (find_block_info) {
			find_block_info = FALSE;

			/* This pack is important since it holds the charset. */
			/* NOTE: it's always the last in a block (max 255
			 * CD-TEXT pack data). So find it first. */
			if (brasero_medium_read_CD_TEXT_block_info (cd_text, i, num, buffer)) {
				BraseroScsiCDTextPackCharset *pack;

				pack = (BraseroScsiCDTextPackCharset *) buffer;
				charset = pack->charset;

				BRASERO_MEDIA_LOG ("Found language pack. Charset = %d. Start %d. End %d",
						  charset, pack->first_track, pack->last_track);
			}
		}

		track_num = cd_text->pack [i].track_num;
		is_double_byte = cd_text->pack [i].double_byte;

		for (j = 0; j < sizeof (cd_text->pack [i].text); j++) {
			if (!off
			&&   cd_text->pack [i].text [j] == '\t'
			&& (!is_double_byte 
			|| (j+1 < sizeof (cd_text->pack [i].text) && cd_text->pack [i].text [j + 1] == '\t'))) {
				/* Specs say that tab character means that's the
				 * same string as before. So if buffer is not
				 * empty send the same string. */
				if (buffer [0] != '\0')
					brasero_medium_get_CD_TEXT (self,
								    cd_text->pack [i].type,
								    track_num,
								    charset,
								    cd_text->pack [i].double_byte,
								    buffer);
				track_num ++;
				continue;
			}

			buffer [off] = cd_text->pack [i].text [j];
			off++;

			if (cd_text->pack [i].text [j] == '\0'
			&& (!is_double_byte 
			|| (j+1 < sizeof (cd_text->pack [i].text) && cd_text->pack [i].text [j + 1] == '\0'))) {
				/* Make sure we actually wrote something to the
				 * buffer and that it's not empty. */
				if (buffer [0] != '\0')
					brasero_medium_get_CD_TEXT (self,
								    cd_text->pack [i].type,
								    track_num,
								    charset,
								    cd_text->pack [i].double_byte,
								    buffer);

				/* End of encapsulated Text Pack. Skip to the next. */
				track_num ++;
				off = 0;
			}
		}
	}

	g_free (cd_text);
}

static void
brasero_medium_init_real (BraseroMedium *object,
			  BraseroDeviceHandle *handle)
{
	guint i;
	gchar *name;
	BraseroBurnResult result;
	BraseroMediumPrivate *priv;
	BraseroScsiErrCode code = 0;
	gchar buffer [256] = { 0, };

	priv = BRASERO_MEDIUM_PRIVATE (object);

	name = brasero_drive_get_display_name (priv->drive);
	BRASERO_MEDIA_LOG ("Initializing information for medium in %s", name);
	g_free (name);

	if (priv->probe_cancelled)
		return;

	result = brasero_medium_get_medium_type (object, handle, &code);
	if (result != BRASERO_BURN_OK)
		return;

	if (priv->probe_cancelled)
		return;

	result = brasero_medium_get_speed (object, handle, &code);
	if (result != BRASERO_BURN_OK)
		return;

	if (priv->probe_cancelled)
		return;

	brasero_medium_get_capacity_by_type (object, handle, &code);
	if (priv->probe_cancelled)
		return;

	result = brasero_medium_get_contents (object, handle, &code);
	if (result != BRASERO_BURN_OK)
		return;

	if (priv->probe_cancelled)
		return;

	/* assume that css feature is only for DVD-ROM which might be wrong but
	 * some drives wrongly reports that css is enabled for blank DVD+R/W */
	if (BRASERO_MEDIUM_IS (priv->info, (BRASERO_MEDIUM_DVD|BRASERO_MEDIUM_ROM)))
		brasero_medium_get_css_feature (object, handle, &code);

	if (priv->probe_cancelled)
		return;

	brasero_medium_init_caps (object, handle, &code);
	if (priv->probe_cancelled)
		return;

	/* read CD-TEXT title */
	if (priv->info & BRASERO_MEDIUM_HAS_AUDIO)
		brasero_medium_read_CD_TEXT (object, handle, &code);

	if (priv->probe_cancelled)
		return;

	brasero_media_to_string (priv->info, buffer);
	BRASERO_MEDIA_LOG ("media is %s", buffer);

	if (!priv->wr_speeds)
		return;

	/* sort write speeds */
	for (i = 0; priv->wr_speeds [i] != 0; i ++) {
		guint j;

		for (j = 0; priv->wr_speeds [j] != 0; j ++) {
			if (priv->wr_speeds [i] > priv->wr_speeds [j]) {
				gint64 tmp;

				tmp = priv->wr_speeds [i];
				priv->wr_speeds [i] = priv->wr_speeds [j];
				priv->wr_speeds [j] = tmp;
			}
		}
	}
}

static gboolean
brasero_medium_probed (gpointer data)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (data);

	/* This signal must be emitted in the main thread */
	GDK_THREADS_ENTER ();
	g_signal_emit (data,
		       medium_signals [PROBED],
		       0);
	GDK_THREADS_LEAVE ();

	priv->probe_id = 0;
	return FALSE;
}

static gpointer
brasero_medium_probe_thread (gpointer self)
{
	gint counter = 0;
	const gchar *path;
	BraseroScsiErrCode code;
	BraseroMediumPrivate *priv;
	BraseroDeviceHandle *handle;

	priv = BRASERO_MEDIUM_PRIVATE (self);
	path = brasero_drive_get_device (priv->drive);

	priv->info = BRASERO_MEDIUM_BUSY;

	/* the drive might be busy (a burning is going on) so we don't block
	 * but we re-try to open it every second */
	BRASERO_MEDIA_LOG ("Trying to open device %s", path);

	handle = brasero_device_handle_open (path, FALSE, &code);
	while (!handle && counter <= BRASERO_MEDIUM_OPEN_ATTEMPTS) {
		sleep (1);

		if (priv->probe_cancelled) {
			priv->probe = NULL;
			return NULL;
		}

		counter ++;
		handle = brasero_device_handle_open (path, FALSE, &code);
	}

	if (priv->probe_cancelled) {
		priv->probe = NULL;
		return NULL;
	}

	if (handle) {
		BRASERO_MEDIA_LOG ("Open () succeeded");

		/* NOTE: if we wanted to know the status we'd need to read the 
		 * error code variable which is currently NULL */
		while (brasero_spc1_test_unit_ready (handle, NULL) != BRASERO_SCSI_OK) {
			sleep (1);

			if (priv->probe_cancelled) {
				priv->probe = NULL;
				brasero_device_handle_close (handle);
				BRASERO_MEDIA_LOG ("Device probing cancelled");
				return NULL;
			}
		}

		BRASERO_MEDIA_LOG ("Device ready");

		brasero_medium_init_real (BRASERO_MEDIUM (self), handle);
		brasero_device_handle_close (handle);
	}
	else
		BRASERO_MEDIA_LOG ("Open () failed: medium busy");

	priv->probe_id = g_idle_add (brasero_medium_probed, self);
	priv->probe = NULL;
	return NULL;
}

static void
brasero_medium_probe (BraseroMedium *self)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	/* NOTE: why a thread? Because in case of a damaged medium, brasero can
	 * block on some functions until timeout and if we do this in the main
	 * thread then our whole UI blocks. This medium won't be exported by the
	 * BraseroDrive that exported until it returns PROBED signal.
	 * One (good) side effect is that it also improves start time. */
	priv->probe = g_thread_create (brasero_medium_probe_thread,
				       self,
				       TRUE,
				       NULL);
}

static void
brasero_medium_init_file (BraseroMedium *self)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (self);

	priv->info = BRASERO_MEDIUM_FILE;
	priv->type = types [0];
}

static void
brasero_medium_init (BraseroMedium *object)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (object);
	priv->next_wr_add = -1;

	/* we can't do anything here since properties haven't been set yet */
}

static void
brasero_medium_finalize (GObject *object)
{
	BraseroMediumPrivate *priv;

	priv = BRASERO_MEDIUM_PRIVATE (object);

	if (priv->probe) {
		priv->probe_cancelled = TRUE;
		g_thread_join (priv->probe);
		priv->probe = 0;
	}

	if (priv->probe_id) {
		g_source_remove (priv->probe_id);
		priv->probe_id = 0;
	}

	if (priv->id) {
		g_free (priv->id);
		priv->id = NULL;
	}

	if (priv->CD_TEXT_title) {
		g_free (priv->CD_TEXT_title);
		priv->CD_TEXT_title = NULL;
	}

	g_free (priv->rd_speeds);
	priv->rd_speeds = NULL;

	g_free (priv->wr_speeds);
	priv->wr_speeds = NULL;

	g_slist_foreach (priv->tracks, (GFunc) g_free, NULL);
	g_slist_free (priv->tracks);
	priv->tracks = NULL;

	priv->drive = NULL;

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
brasero_medium_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	BraseroMediumPrivate *priv;

	g_return_if_fail (BRASERO_IS_MEDIUM (object));

	priv = BRASERO_MEDIUM_PRIVATE (object);

	switch (prop_id)
	{
	case PROP_DRIVE:
		/* we don't ref the drive here as it would create a circular
		 * dependency where the drive would hold a reference on the 
		 * medium and the medium on the drive */
		priv->drive = g_value_get_object (value);

		if (brasero_drive_is_fake (priv->drive)) {
			brasero_medium_init_file (BRASERO_MEDIUM (object));
			break;
		}

		brasero_medium_probe (BRASERO_MEDIUM (object));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
brasero_medium_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	BraseroMediumPrivate *priv;

	g_return_if_fail (BRASERO_IS_MEDIUM (object));

	priv = BRASERO_MEDIUM_PRIVATE (object);

	switch (prop_id)
	{
	case PROP_DRIVE:
		g_value_set_object (value, priv->drive);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
brasero_medium_class_init (BraseroMediumClass *klass)
{
	GObjectClass* object_class = G_OBJECT_CLASS (klass);
	parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));

	g_type_class_add_private (klass, sizeof (BraseroMediumPrivate));

	object_class->finalize = brasero_medium_finalize;
	object_class->set_property = brasero_medium_set_property;
	object_class->get_property = brasero_medium_get_property;

	/**
 	* BraseroMedium::probed:
 	* @medium: the object which received the signal
	*
 	* This signal gets emitted when the medium inside the drive has been
	* fully probed. This is mostly for internal use.
 	*
 	*/
	medium_signals[PROBED] =
		g_signal_new ("probed",
		              G_OBJECT_CLASS_TYPE (klass),
		              G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE,
		              0,
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0,
		              G_TYPE_NONE);

	g_object_class_install_property (object_class,
	                                 PROP_DRIVE,
	                                 g_param_spec_object ("drive",
	                                                      "Drive",
	                                                      "Drive in which medium is inserted",
	                                                      BRASERO_TYPE_DRIVE,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

/**
 * brasero_medium_can_be_written:
 * @medium: #BraseroMedium
 *
 * Gets whether the medium can be written. It also checks that the medium can
 * write the medium.
 *
 * Return value: a #gboolean. TRUE if the medium can be rewritten, FALSE otherwise.
 *
 **/
gboolean
brasero_medium_can_be_written (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;
	BraseroDriveCaps caps;

	g_return_val_if_fail (medium != NULL, FALSE);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), FALSE);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	if (!(priv->info & BRASERO_MEDIUM_REWRITABLE)
	&&   (priv->info & BRASERO_MEDIUM_CLOSED))
		return FALSE;

	if (priv->info & BRASERO_MEDIUM_FILE)
		return FALSE;

	caps = brasero_drive_get_caps (priv->drive);
	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDR))
		return (caps & BRASERO_DRIVE_CAPS_CDR) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDR))
		return (caps & BRASERO_DRIVE_CAPS_DVDR) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDR_PLUS))
		return (caps & BRASERO_DRIVE_CAPS_DVDR_PLUS) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDRW))
		return (caps & BRASERO_DRIVE_CAPS_CDRW) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_RESTRICTED))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_PLUS))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW_PLUS) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDR_PLUS_DL))
		return (caps & BRASERO_DRIVE_CAPS_DVDR_PLUS_DL) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_PLUS_DL))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW_PLUS_DL) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVD_RAM))
		return (caps & BRASERO_DRIVE_CAPS_DVDRAM) != 0;

	/* All types of BD-R */
	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BD|BRASERO_MEDIUM_WRITABLE))
		return (caps & BRASERO_DRIVE_CAPS_BDR) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BDRE))
		return (caps & BRASERO_DRIVE_CAPS_BDRW) != 0;

	return FALSE;
}

/**
 * brasero_medium_can_be_rewritten:
 * @medium: #BraseroMedium
 *
 * Gets whether the medium can be rewritten. Note: it also checks that the drive
 * can rewrite the medium type.
 *
 * Return value: a #gboolean. TRUE if the medium can be rewritten, FALSE otherwise.
 *
 **/
gboolean
brasero_medium_can_be_rewritten (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;
	BraseroDriveCaps caps;

	g_return_val_if_fail (medium != NULL, FALSE);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), FALSE);

	priv = BRASERO_MEDIUM_PRIVATE (medium);

	if (!(priv->info & BRASERO_MEDIUM_REWRITABLE)
	||   (priv->info & BRASERO_MEDIUM_FILE))
		return FALSE;

	caps = brasero_drive_get_caps (priv->drive);
	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_CDRW))
		return (caps & BRASERO_DRIVE_CAPS_CDRW) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_RESTRICTED))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_PLUS))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW_PLUS) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVDRW_PLUS_DL))
		return (caps & BRASERO_DRIVE_CAPS_DVDRW_PLUS_DL) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_DVD_RAM))
		return (caps & BRASERO_DRIVE_CAPS_DVDRAM) != 0;

	if (BRASERO_MEDIUM_IS (priv->info, BRASERO_MEDIUM_BDRE))
		return (caps & BRASERO_DRIVE_CAPS_BDRW) != 0;

	return FALSE;
}

/**
 * brasero_medium_get_drive:
 * @medium: #BraseroMedium
 *
 * Gets the #BraseroDrive in which the medium is inserted.
 *
 * Return value: a #BraseroDrive. No need to unref after use.
 *
 **/
BraseroDrive *
brasero_medium_get_drive (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;

	if (!medium)
		return NULL;

	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), NULL);

	priv = BRASERO_MEDIUM_PRIVATE (medium);
	return priv->drive;
}

/**
 * brasero_medium_get_CD_TEXT_title:
 * @medium: #BraseroMedium
 *
 * Gets the CD-TEXT title for @Medium.
 *
 * Return value: a #gchar *.
 *
 **/
const gchar *
brasero_medium_get_CD_TEXT_title (BraseroMedium *medium)
{
	BraseroMediumPrivate *priv;

	g_return_val_if_fail (medium != NULL, NULL);
	g_return_val_if_fail (BRASERO_IS_MEDIUM (medium), NULL);

	priv = BRASERO_MEDIUM_PRIVATE (medium);
	return priv->CD_TEXT_title;

}

GType
brasero_medium_get_type (void)
{
	static GType our_type = 0;

	if (our_type == 0)
	{
		static const GTypeInfo our_info =
		{
			sizeof (BraseroMediumClass), /* class_size */
			(GBaseInitFunc) NULL, /* base_init */
			(GBaseFinalizeFunc) NULL, /* base_finalize */
			(GClassInitFunc) brasero_medium_class_init, /* class_init */
			(GClassFinalizeFunc) NULL, /* class_finalize */
			NULL /* class_data */,
			sizeof (BraseroMedium), /* instance_size */
			0, /* n_preallocs */
			(GInstanceInitFunc) brasero_medium_init, /* instance_init */
			NULL /* value_table */
		};

		our_type = g_type_register_static (G_TYPE_OBJECT, "BraseroMedium",
		                                   &our_info, 0);
	}

	return our_type;
}
