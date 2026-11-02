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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/metadata/segtype.h"
#include "lib/format_text/text_export.h"
#include "lib/config/config.h"
#include "lib/datastruct/str_list.h"
#include "lib/misc/lvm-string.h"
#include "lib/activate/activate.h"
#include "lib/metadata/metadata.h"
#include "lib/metadata/lv_alloc.h"
#include "lib/config/defaults.h"

#define SEG_LOG_ERROR(t, p...) \
        log_error(t " segment %s of logical volume %s.", ## p,	\
                  dm_config_parent_name(sn), seg->lv->name), 0;

static void _writeboost_display(const struct lv_segment *seg)
{
	/* TODO: lvdisplay segments */
}

static int _writeboost_text_import(struct lv_segment *seg,
				   const struct dm_config_node *sn)
{
	struct logical_volume *origin_lv = NULL;
	struct logical_volume *cache_lv;
	const char *origin_name = NULL;
	const char *cache_name = NULL;

	if (!dm_config_has_node(sn, "origin"))
		return SEG_LOG_ERROR("origin not specified in");

	if (!dm_config_get_str(sn, "origin", &origin_name))
		return SEG_LOG_ERROR("origin must be a string in");

	if (!(origin_lv = find_lv(seg->lv->vg, origin_name)))
		return SEG_LOG_ERROR("Unknown LV specified for writeboost origin %s in", origin_name);

	if (!set_lv_segment_area_lv(seg, 0, origin_lv, 0, 0))
		return_0;

	if (!dm_config_has_node(sn, "writeboost"))
		return SEG_LOG_ERROR("writeboost not specified in");

	if (!dm_config_get_str(sn, "writeboost", &cache_name))
		return SEG_LOG_ERROR("writeboost must be a string in");

	if (!(cache_lv = find_lv(seg->lv->vg, cache_name)))
		return SEG_LOG_ERROR("Unknown logical volume %s specified for writeboost in",
				     cache_name);

	seg->origin = origin_lv;
	seg->writeboost = cache_lv;
	seg->lv->status |= WRITECACHEBOOST;

	seg->writeboost_settings.writeback_threshold = 70;
	seg->writeboost_settings.nr_max_batched_writeback = 32;
	seg->writeboost_settings.update_sb_record_interval = 0;
	seg->writeboost_settings.sync_data_interval = 3600;
	seg->writeboost_settings.read_cache_threshold = 0;
	seg->writeboost_settings.write_around_mode = 0;
	seg->writeboost_settings.nr_read_cache_cells = 2048;

	if (dm_config_has_node(sn, "writeback_threshold") &&
	    !dm_config_get_uint32(sn, "writeback_threshold", &seg->writeboost_settings.writeback_threshold))
		return SEG_LOG_ERROR("Unknown writeback_threshold in");

	if (dm_config_has_node(sn, "nr_max_batched_writeback") &&
	    !dm_config_get_uint32(sn, "nr_max_batched_writeback", &seg->writeboost_settings.nr_max_batched_writeback))
		return SEG_LOG_ERROR("Unknown nr_max_batched_writeback in");

	if (dm_config_has_node(sn, "update_sb_record_interval") &&
	    !dm_config_get_uint32(sn, "update_sb_record_interval", &seg->writeboost_settings.update_sb_record_interval))
		return SEG_LOG_ERROR("Unknown update_sb_record_interval in");

	if (dm_config_has_node(sn, "sync_data_interval") &&
	    !dm_config_get_uint32(sn, "sync_data_interval", &seg->writeboost_settings.sync_data_interval))
		return SEG_LOG_ERROR("Unknown sync_data_interval in");

	if (dm_config_has_node(sn, "read_cache_threshold") &&
	    !dm_config_get_uint32(sn, "read_cache_threshold", &seg->writeboost_settings.read_cache_threshold))
		return SEG_LOG_ERROR("Unknown read_cache_threshold in");

	if (dm_config_has_node(sn, "write_around_mode") &&
	    !dm_config_get_uint32(sn, "write_around_mode", &seg->writeboost_settings.write_around_mode))
		return SEG_LOG_ERROR("Unknown write_around_mode in");

	if (dm_config_has_node(sn, "nr_read_cache_cells") &&
	    !dm_config_get_uint32(sn, "nr_read_cache_cells", &seg->writeboost_settings.nr_read_cache_cells))
		return SEG_LOG_ERROR("Unknown nr_read_cache_cells in");

	if (!add_seg_to_segs_using_this_lv(cache_lv, seg))
		return_0;

	return 1;
}

static int _writeboost_text_import_area_count(const struct dm_config_node *sn,
					      uint32_t *area_count)
{
	*area_count = 1;

	return 1;
}

int writeboost_settings_to_str_list(const struct dm_writeboost_settings *settings,
				    struct dm_list *result, struct dm_pool *mem)
{
	int errors = 0;

	if (!setting_str_list_add("writeback_threshold", settings->writeback_threshold, NULL, result, mem))
		errors++;

	if (!setting_str_list_add("nr_max_batched_writeback", settings->nr_max_batched_writeback, NULL, result, mem))
		errors++;

	if (!setting_str_list_add("update_sb_record_interval", settings->update_sb_record_interval, NULL, result, mem))
		errors++;

	if (!setting_str_list_add("sync_data_interval", settings->sync_data_interval, NULL, result, mem))
		errors++;

	if (!setting_str_list_add("read_cache_threshold", settings->read_cache_threshold, NULL, result, mem))
		errors++;

	if (!setting_str_list_add("write_around_mode", settings->write_around_mode, NULL, result, mem))
		errors++;

	if (!setting_str_list_add("nr_read_cache_cells", settings->nr_read_cache_cells, NULL, result, mem))
		errors++;

	if (errors)
		log_warn("WARNING: Failed to create list of writeboost settings.");

	return 1;
}

static int _writeboost_text_export(const struct lv_segment *seg,
				   struct formatter *f)
{
	outf(f, "writeboost = \"%s\"", seg->writeboost->name);
	outf(f, "origin = \"%s\"", seg_lv(seg, 0)->name);
	outf(f, "writeback_threshold = %u", seg->writeboost_settings.writeback_threshold);
	outf(f, "nr_max_batched_writeback = %u", seg->writeboost_settings.nr_max_batched_writeback);
	outf(f, "update_sb_record_interval = %u", seg->writeboost_settings.update_sb_record_interval);
	outf(f, "sync_data_interval = %u", seg->writeboost_settings.sync_data_interval);
	outf(f, "read_cache_threshold = %u", seg->writeboost_settings.read_cache_threshold);
	outf(f, "write_around_mode = %u", seg->writeboost_settings.write_around_mode);
	outf(f, "nr_read_cache_cells = %u", seg->writeboost_settings.nr_read_cache_cells);

	return 1;
}

static void _destroy(struct segment_type *segtype)
{
	free((void *) segtype);
}

#ifdef DEVMAPPER_SUPPORT

static int _target_present(struct cmd_context *cmd,
			   const struct lv_segment *seg __attribute__((unused)),
			   unsigned *attributes __attribute__((unused)))
{
	static int _writeboost_checked = 0;
	static int _writeboost_present = 0;
	uint32_t maj, min, patchlevel;

	if (!activation())
		return 0;

	if (!_writeboost_checked) {
		_writeboost_checked = 1;
		if (!(_writeboost_present = target_present_version(cmd, TARGET_NAME_WRITECACHEBOOST, 1,
								   &maj, &min, &patchlevel)))
			return 0;

		if (maj < 1) {
			log_error("dm-writeboost module version older than minimum 1.0.0");
			return 0;
		}
	}

	return _writeboost_present;
}

static int _modules_needed(struct dm_pool *mem,
			   const struct lv_segment *seg __attribute__((unused)),
			   struct dm_list *modules)
{
	if (!str_list_add(mem, modules, MODULE_NAME_WRITECACHEBOOST)) {
		log_error("String list allocation failed for writeboost module.");
		return 0;
	}

	return 1;
}

static int _writeboost_add_target_line(struct dev_manager *dm,
				 struct dm_pool *mem,
				 struct cmd_context *cmd __attribute__((unused)),
				 void **target_state __attribute__((unused)),
				 struct lv_segment *seg,
				 const struct lv_activate_opts *laopts __attribute__((unused)),
				 struct dm_tree_node *node, uint64_t len,
				 uint32_t *pvmove_mirror_count __attribute__((unused)))
{
	char *origin_uuid;
	char *cache_uuid;
	struct dm_writeboost_settings settings = seg->writeboost_settings;

	if (!seg_is_writeboost(seg)) {
		log_error(INTERNAL_ERROR "Passed segment is not writeboost.");
		return 0;
	}

	if (!seg->writeboost) {
		log_error(INTERNAL_ERROR "Passed segment has no writeboost.");
		return 0;
	}

	if (!(origin_uuid = build_dm_uuid(mem, seg_lv(seg, 0), "real")))
		return_0;

	if (!(cache_uuid = build_dm_uuid(mem, seg->writeboost, "cvol")))
		return_0;

	if (!dm_tree_node_add_writeboost_target(node, len,
						origin_uuid, cache_uuid,
						&settings))
		return_0;

	return 1;
}
#else
static int _target_present(struct cmd_context *cmd,
			   const struct lv_segment *seg __attribute__((unused)),
			   unsigned *attributes __attribute__((unused)))
{
	return 0;
}
#endif /* DEVMAPPER_SUPPORT */

static const struct segtype_handler _writeboost_ops = {
	.display = _writeboost_display,
	.text_import = _writeboost_text_import,
	.text_import_area_count = _writeboost_text_import_area_count,
	.text_export = _writeboost_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _writeboost_add_target_line,
	.target_present = _target_present,
	.modules_needed = _modules_needed,
#endif
	.destroy = _destroy,
};

int init_writeboost_segtypes(struct cmd_context *cmd,
			struct segtype_library *seglib)
{
	struct segment_type *segtype = zalloc(sizeof(*segtype));

	if (!segtype) {
		log_error("Failed to allocate memory for writeboost segtype");
		return 0;
	}

	segtype->name = SEG_TYPE_NAME_WRITECACHEBOOST;
	segtype->flags = SEG_WRITECACHEBOOST;
	segtype->ops = &_writeboost_ops;

	if (!lvm_register_segtype(seglib, segtype))
		return_0;
	log_very_verbose("Initialised segtype: %s", segtype->name);

	return 1;
}
