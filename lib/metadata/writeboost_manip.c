/*
 * Copyright (C) 2024 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "lib/locking/locking.h"
#include "lib/misc/lvm-string.h"
#include "lib/commands/toolcontext.h"
#include "lib/display/display.h"
#include "lib/metadata/segtype.h"
#include "lib/activate/activate.h"
#include "lib/config/defaults.h"

int lv_is_writeboost_origin(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	if (lv->status & WRITECACHEBOOST_ORIGIN)
		return 1;

	if (dm_list_size(&lv->segs_using_this_lv) != 1)
		return 0;

	seg = get_only_segment_using_this_lv(lv);
	return seg && lv_is_writeboost(seg->lv) && !lv_is_pending_delete(seg->lv) && (seg_lv(seg, 0) == lv);
}

int lv_is_writeboost_cachevol(const struct logical_volume *lv)
{
	struct seg_list *sl;

	dm_list_iterate_items(sl, &lv->segs_using_this_lv) {
		if (!sl->seg || !sl->seg->lv || !sl->seg->writeboost)
			continue;
		if (lv_is_writeboost(sl->seg->lv) && (sl->seg->writeboost == lv))
			return 1;
	}
	return 0;
}

static void _rename_detached_cvol(struct cmd_context *cmd, struct logical_volume *lv_fast)
{
	struct volume_group *vg = lv_fast->vg;
	char cvol_name[NAME_LEN];
	char *suffix, *cvol_name_dup;

	if (!_dm_strncpy(cvol_name, lv_fast->name, sizeof(cvol_name)) ||
	    !(suffix = strstr(cvol_name, "_cvol"))) {
		log_debug("LV %s has no suffix for cachevol (skipping rename).",
			display_lvname(lv_fast));
		return;
	}

	*suffix = 0;
	if (lv_name_is_used_in_vg(vg, cvol_name, NULL) &&
	    !generate_lv_name(vg, "lvol%d", cvol_name, sizeof(cvol_name))) {
		log_warn("Failed to generate new unique name for unused LV %s", lv_fast->name);
		return;
	}

	if (!(cvol_name_dup = dm_pool_strdup(vg->vgmem, cvol_name))) {
		stack;
		return;
	}

	if (!lv_set_name(lv_fast, cvol_name_dup))
		stack;
}

static int _lv_detach_writeboost_cachevol_inactive(struct logical_volume *lv, int noflush)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_fast;
	struct logical_volume *lv_wborig;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_writeboost(seg)) {
		log_error("LV %s segment is not writeboost.", display_lvname(lv));
		return 0;
	}

	if (!(lv_fast = seg->writeboost)) {
		log_error("LV %s writeboost segment has no cachevol.", display_lvname(lv));
		return 0;
	}

	if (!(lv_wborig = seg_lv(seg, 0))) {
		log_error("LV %s writeboost segment has no origin.", display_lvname(lv));
		return 0;
	}

	if (noflush)
		goto detach;

	if (!activate_lv_temporary(cmd, lv)) {
		log_error("Failed to activate LV %s for flushing writeboost.", display_lvname(lv));
		return 0;
	}

	if (!lv_writeboost_message(lv, "drop_caches")) {
		log_error("Failed to flush writeboost for %s.", display_lvname(lv));
		if (!deactivate_lv(cmd, lv))
			log_error("Failed to deactivate %s.", display_lvname(lv));
		return 0;
	}

	if (!deactivate_lv(cmd, lv)) {
		log_error("Failed to deactivate LV %s for detaching writeboost.", display_lvname(lv));
		return 0;
	}

detach:
	if (!remove_seg_from_segs_using_this_lv(lv_fast, seg))
		return_0;

	lv->status &= ~WRITECACHEBOOST;
	seg->writeboost = NULL;

	if (!remove_layer_from_lv(lv, lv_wborig))
		return_0;

	if (!lv_remove(lv_wborig))
		return_0;

	lv_set_visible(lv_fast);
	lv_fast->status &= ~LV_CACHE_VOL;
	_rename_detached_cvol(cmd, lv_fast);

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	return 1;
}

static int _lv_detach_writeboost_cachevol_active(struct logical_volume *lv, int noflush)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_fast;
	struct logical_volume *lv_wborig;
	struct lv_segment *seg = first_seg(lv);

	if (!seg_is_writeboost(seg)) {
		log_error("LV %s segment is not writeboost.", display_lvname(lv));
		return 0;
	}

	if (!(lv_fast = seg->writeboost)) {
		log_error("LV %s writeboost segment has no cachevol.", display_lvname(lv));
		return 0;
	}

	if (!(lv_wborig = seg_lv(seg, 0))) {
		log_error("LV %s writeboost segment has no origin.", display_lvname(lv));
		return 0;
	}

	if (!noflush) {
		if (!suspend_lv(cmd, lv)) {
			log_error("Failed to suspend LV %s for writeboost flush.", display_lvname(lv));
			return 0;
		}

		if (!resume_lv(cmd, lv)) {
			log_error("Failed to resume LV %s for writeboost flush.", display_lvname(lv));
			return 0;
		}

		if (!lv_writeboost_message(lv, "drop_caches")) {
			log_error("Failed to flush writeboost for %s.", display_lvname(lv));
			return 0;
		}
	}

	if (!remove_seg_from_segs_using_this_lv(lv_fast, seg)) {
		log_error("Failed to remove seg in writeboost detach.");
		return 0;
	}

	lv->status &= ~WRITECACHEBOOST;
	seg->writeboost = NULL;

	if (!remove_layer_from_lv(lv, lv_wborig)) {
		log_error("Failed to remove lv layer in writeboost detach.");
		return 0;
	}

	if (!vg_write(vg)) {
		log_error("Failed to write VG in writeboost detach.");
		return 0;
	}

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to suspend LV in writeboost detach.");
		vg_revert(vg);
		return 0;
	}

	if (!vg_commit(vg)) {
		log_error("Failed to commit VG in writeboost detach.");
		return 0;
	}

	if (!resume_lv(cmd, lv)) {
		log_error("Failed to resume LV in writeboost detach.");
		return 0;
	}

	sync_local_dev_names(cmd);

	if (!deactivate_lv(cmd, lv_fast))
		log_error("Failed to deactivate previous cachevol in writeboost detach.");

	lv_wborig->status |= WRITECACHEBOOST_ORIGIN;

	if (!lv_deactivate(cmd, NULL, lv_wborig))
		log_error("Failed to deactivate previous wborig LV in writeboost detach.");

	if (!lv_remove(lv_wborig)) {
		log_error("Failed to remove previous wborig LV in writeboost detach.");
		return 0;
	}

	lv_set_visible(lv_fast);
	lv_fast->status &= ~LV_CACHE_VOL;
	_rename_detached_cvol(cmd, lv_fast);

	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("Failed to write and commit VG in writeboost detach.");
		return 0;
	}

	return 1;
}

int lv_detach_writeboost_cachevol(struct logical_volume *lv, int noflush)
{
	if (lv_is_active(lv))
		return _lv_detach_writeboost_cachevol_active(lv, noflush);
	else
		return _lv_detach_writeboost_cachevol_inactive(lv, noflush);
}
