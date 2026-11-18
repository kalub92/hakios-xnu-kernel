/*
 * Made by a program, not by hand: the tables of Linux v7.3's drivers/gpu/drm/imagination/pvr_stream_defs.c
 * (GPL-2.0-only OR MIT) and the offsets, sizes and values the job code takes from pvr_rogue_fwif.h,
 * pvr_rogue_fwif_client.h and pvr_rogue_fwif_shared.h, each computed by compiling those headers for a
 * little-endian 64-bit host and printing offsetof/sizeof (the generator is in the bring-up's scratch
 * directory, gen/gen.c). A stream entry: where it goes in the firmware's command, how many bytes,
 * the feature it depends on and whether it is there only WITHOUT that feature.
 */
static const struct stream_def stream_geom_main[] = {
	{ 16, 8, SF_NONE, false },
	{ 24, 8, SF_NONE, false },
	{ 32, 8, F_VDM_DRAWINDIRECT, false },
	{ 40, 4, F_VDM_DRAWINDIRECT, false },
	{ 44, 4, SF_NONE, false },
	{ 48, 4, SF_NONE, false },
	{ 56, 4, SF_NONE, false },
	{ 60, 4, F_VDM_OBJECT_LEVEL_LLS, false },
	{ 68, 4, SF_NONE, false },
	{ 72, 4, F_TESSELLATION, false },
};
static const struct stream_def stream_geom_ext0_49927[] = {
	{ 52, 4, SF_NONE, false },
};
static const struct stream_ext stream_geom_ext[] = {
	{ stream_geom_ext0_49927, 1, 0, 0x1, 49927, 0x1 },
};
#define stream_geom_DEST_SIZE 112
#define stream_geom_EXT_HEADERS 1

static const struct stream_def stream_frag_main[] = {
	{ 112, 8, SF_NONE, false },
	{ 120, 8, SF_NONE, false },
	{ 128, 8, SF_NONE, false },
	{ 136, 8, SF_NONE, false },
	{ 144, 8, SF_NONE, false },
	{ 152, 8, SF_NONE, false },
	{ 160, 8, SF_REQUIRES_FB_CDC_ZLS_SETUP, false },
	{ 168, 192, SF_NONE, false },
	{ 360, 8, SF_NONE, false },
	{ 368, 24, SF_NONE, false },
	{ 416, 24, SF_NONE, false },
	{ 20, 32, SF_NONE, false },
	{ 16, 4, SF_NONE, false },
	{ 52, 4, SF_NONE, false },
	{ 56, 4, SF_NONE, false },
	{ 60, 4, SF_NONE, false },
	{ 64, 4, F_S7_TOP_INFRASTRUCTURE, false },
	{ 68, 4, SF_NONE, false },
	{ 76, 4, SF_NONE, false },
	{ 80, 4, F_CLUSTER_GROUPING, false },
	{ 84, 4, SF_NONE, false },
	{ 88, 4, SF_NONE, false },
	{ 96, 4, F_GPU_MULTICORE_SUPPORT, false },
	{ 100, 4, F_ZLS_SUBTILE, false },
	{ 104, 4, F_ISP_ZLS_D24_S8_PACKING_OGL_MODE, false },
	{ 468, 4, SF_NONE, false },
	{ 472, 4, SF_NONE, false },
	{ 476, 4, F_GPU_MULTICORE_SUPPORT, false },
};
static const struct stream_def stream_frag_ext0_47217[] = {
	{ 96, 4, SF_NONE, false },
};
static const struct stream_def stream_frag_ext0_49927[] = {
	{ 72, 4, SF_NONE, false },
};
static const struct stream_ext stream_frag_ext[] = {
	{ stream_frag_ext0_47217, 1, 0, 0x1, 47217, 0x2 },
	{ stream_frag_ext0_49927, 1, 0, 0x2, 49927, 0x2 },
};
#define stream_frag_DEST_SIZE 480
#define stream_frag_EXT_HEADERS 1

static const struct stream_def stream_compute_main[] = {
	{ 8, 8, SF_NONE, false },
	{ 16, 8, SF_CDM_USER_MODE_QUEUE, false },
	{ 24, 8, SF_CDM_USER_MODE_QUEUE, false },
	{ 32, 8, SF_CDM_USER_MODE_QUEUE, false },
	{ 40, 8, SF_CDM_USER_MODE_QUEUE, true },
	{ 48, 8, SF_NONE, false },
	{ 60, 4, SF_NONE, false },
	{ 64, 4, F_COMPUTE_MORTON_CAPABLE, false },
	{ 68, 4, F_CLUSTER_GROUPING, false },
	{ 72, 4, F_TPU_DM_GLOBAL_REGISTERS, false },
	{ 88, 4, SF_CDM_USER_MODE_QUEUE, false },
	{ 92, 4, F_GPU_MULTICORE_SUPPORT, false },
};
static const struct stream_def stream_compute_ext0_49927[] = {
	{ 56, 4, SF_NONE, false },
};
static const struct stream_ext stream_compute_ext[] = {
	{ stream_compute_ext0_49927, 1, 0, 0x1, 49927, 0x1 },
};
#define stream_compute_DEST_SIZE 96
#define stream_compute_EXT_HEADERS 1

static const struct stream_def stream_transfer_main[] = {
	{ 80, 8, SF_NONE, false },
	{ 88, 8, SF_NONE, false },
	{ 96, 8, SF_NONE, false },
	{ 104, 8, SF_NONE, false },
	{ 112, 72, SF_NONE, false },
	{ 8, 4, SF_NONE, false },
	{ 12, 4, SF_NONE, false },
	{ 16, 4, SF_NONE, false },
	{ 20, 4, SF_NONE, false },
	{ 24, 4, SF_NONE, false },
	{ 28, 4, SF_NONE, false },
	{ 32, 4, SF_NONE, false },
	{ 36, 4, SF_NONE, false },
	{ 40, 4, SF_NONE, false },
	{ 48, 4, SF_NONE, false },
	{ 52, 4, SF_NONE, false },
	{ 56, 4, SF_NONE, false },
	{ 60, 4, SF_NONE, false },
	{ 64, 4, SF_NONE, false },
	{ 68, 4, SF_NONE, false },
	{ 44, 4, F_S7_TOP_INFRASTRUCTURE, false },
	{ 72, 4, F_GPU_MULTICORE_SUPPORT, false },
};
#define stream_transfer_DEST_SIZE 192
#define stream_transfer_EXT_HEADERS 0

static const struct stream_def stream_static_render_main[] = {
	{ 0, 8, SF_NONE, false },
	{ 8, 8, SF_NONE, false },
	{ 16, 8, SF_NONE, false },
	{ 24, 8, SF_NONE, false },
	{ 32, 8, SF_NONE, false },
	{ 40, 8, SF_NONE, false },
	{ 72, 8, SF_NONE, false },
	{ 80, 8, SF_NONE, false },
	{ 48, 8, SF_NONE, false },
	{ 56, 8, SF_NONE, false },
	{ 64, 8, SF_NONE, false },
	{ 88, 8, SF_NONE, false },
	{ 96, 8, SF_NONE, false },
	{ 104, 8, SF_NONE, false },
	{ 112, 8, SF_NONE, false },
	{ 120, 8, SF_NONE, false },
	{ 152, 8, SF_NONE, false },
	{ 160, 8, SF_NONE, false },
	{ 128, 8, SF_NONE, false },
	{ 136, 8, SF_NONE, false },
	{ 144, 8, SF_NONE, false },
	{ 168, 8, SF_NONE, false },
	{ 176, 8, SF_NONE, false },
};
#define stream_static_render_DEST_SIZE 184
#define stream_static_render_EXT_HEADERS 0

static const struct stream_def stream_static_compute_main[] = {
	{ 0, 8, SF_NONE, false },
	{ 8, 8, SF_NONE, false },
	{ 16, 8, SF_NONE, false },
	{ 24, 8, SF_NONE, false },
	{ 32, 8, SF_NONE, false },
	{ 40, 8, SF_NONE, false },
	{ 48, 8, SF_NONE, false },
};
#define stream_static_compute_DEST_SIZE 56
#define stream_static_compute_EXT_HEADERS 0

#define GEN_rogue_fwif_cmd_geom__cmd_shared_cmn_frame_num 0
#define GEN_rogue_fwif_cmd_geom__cmd_shared_hwrt_data_fw_addr 4
#define GEN_rogue_fwif_cmd_geom__flags 80
#define GEN_rogue_fwif_cmd_geom__partial_render_geom_frag_fence 84
#define GEN_rogue_fwif_cmd_frag__cmd_shared_cmn_frame_num 0
#define GEN_rogue_fwif_cmd_frag__cmd_shared_hwrt_data_fw_addr 4
#define GEN_rogue_fwif_cmd_frag__flags 464
#define GEN_rogue_fwif_cmd_compute__common_frame_num 0
#define GEN_rogue_fwif_cmd_compute__flags 80
#define GEN_rogue_fwif_cmd_transfer__common_frame_num 0
#define GEN_rogue_fwif_cmd_transfer__flags 184
#define GEN_ROGUE_GEOM_FLAGS_FIRSTKICK 0x1
#define GEN_ROGUE_GEOM_FLAGS_LASTKICK 0x2
#define GEN_ROGUE_GEOM_FLAGS_SINGLE_CORE 0x8
#define GEN_ROGUE_FRAG_FLAGS_SINGLE_CORE 0x8
#define GEN_ROGUE_FRAG_FLAGS_DEPTHBUFFER 0x80
#define GEN_ROGUE_FRAG_FLAGS_STENCILBUFFER 0x100
#define GEN_ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP 0x4000000
#define GEN_ROGUE_FRAG_FLAGS_SCRATCHBUFFER 0x80000
#define GEN_ROGUE_FRAG_FLAGS_GET_VIS_RESULTS 0x20
#define GEN_ROGUE_FRAG_FLAGS_DISABLE_PIXELMERGE 0x8000
#define GEN_ROGUE_COMPUTE_FLAG_PREVENT_ALL_OVERLAP 0x4
#define GEN_ROGUE_COMPUTE_FLAG_SINGLE_CORE 0x20
#define GEN_ROGUE_TRANSFER_FLAGS_SINGLE_CORE 0x2
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_GEOM 0x2abc80c9
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_FRAG 0x2abc80cb
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR 0x2abc80cc
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_CDM 0x2abc80cd
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D 0x2abc80ca
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_FENCE_PR 0x2abc00d7
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_UPDATE 0x2abc00d5
#define GEN_ROGUE_FWIF_CCB_CMD_TYPE_PADDING 0x2abc00dd
#define GEN_ROGUE_FWIF_CCB_CMD_MAX_UFOS 0x40
#define GEN_ROGUE_FWIF_KCCB_CMD_KICK 0x2abc0065
#define GEN_ROGUE_FWIF_KCCB_CMD_COMBINED_GEOM_FRAG_KICK 0x2abc0075
#define GEN_SIZEOF_rogue_fwif_ccb_cmd_header 40
#define GEN_rogue_fwif_ccb_cmd_header__cmd_type 0
#define GEN_rogue_fwif_ccb_cmd_header__cmd_size 4
#define GEN_rogue_fwif_ccb_cmd_header__ext_job_ref 8
#define GEN_rogue_fwif_ccb_cmd_header__int_job_ref 12
#define GEN_SIZEOF_rogue_fwif_kccb_cmd_kick_data 32
#define GEN_rogue_fwif_kccb_cmd_kick_data__context_fw_addr 0
#define GEN_rogue_fwif_kccb_cmd_kick_data__client_woff_update 4
#define GEN_rogue_fwif_kccb_cmd_kick_data__client_wrap_mask_update 8
#define GEN_rogue_fwif_kccb_cmd_kick_data__num_cleanup_ctl 12
#define GEN_rogue_fwif_kccb_cmd_kick_data__cleanup_ctl_fw_addr 16
#define GEN_rogue_fwif_kccb_cmd__cmd_data 8
#define GEN_rogue_fwif_hwrtdata__cleanup_state 320
#define GEN_SIZEOF_rogue_fwif_fwrendercontext 680
#define GEN_rogue_fwif_fwrendercontext__frag_context 152
#define GEN_rogue_fwif_fwrendercontext__static_render_context_state_ctxswitch_regs 304
#define GEN_SIZEOF_rogue_fwif_fwcomputecontext 232
#define GEN_rogue_fwif_fwcomputecontext__cdm_context 0
#define GEN_rogue_fwif_fwcomputecontext__static_compute_context_state_ctxswitch_regs 152
#define GEN_SIZEOF_rogue_fwif_fwtransfercontext 152
#define GEN_rogue_fwif_fwtransfercontext__tq_context 0
#define GEN_SIZEOF_rogue_fwif_compute_ctx_state 4
#define GEN_SIZEOF_rogue_fwif_geom_ctx_state 160
#define GEN_SIZEOF_rogue_fwif_frag_ctx_state 16
#define GEN_SIZEOF_rogue_fwif_fwccb_cmd_context_reset_data 40
#define GEN_rogue_fwif_fwccb_cmd_freelists_reconstruction_data__freelist_count 0
#define GEN_rogue_fwif_fwccb_cmd_freelists_reconstruction_data__freelist_ids 8
#define GEN_ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT 0x12
#define GEN_ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE 0x2abc0070
#define GEN_ROGUE_FWIF_FREELISTS_RECONSTRUCTION_FAILED_FLAG 0x80000000
#define GEN_rogue_fwif_hwrtdata__state 172
#define GEN_rogue_fwif_hwrtdata__hwrt_data_flags 168
#define GEN_ROGUE_FWIF_RTDATA_STATE_HWR 0x9
#define GEN_HWRTDATA_HAS_LAST_GEOM 0x4
#define GEN_rogue_fwif_freelist__allocated_page_count 32
#define GEN_rogue_fwif_freelist__allocated_mmu_page_count 36
#define GEN_rogue_fwif_freelist__current_stack_top 16
#define GEN_ROGUE_PM_MAX_FREELIST_SIZE 0x80000000
#define GEN_ROGUE_PM_PAGE_SIZE 0x1000
#define GEN_ROGUE_MAX_PER_KERNEL_LOCAL_MEM_SIZE_REGS 0x1000
#define GEN_ROGUE_MAX_VERTEX_SHARED_REGISTERS 0x400
#define GEN_ROGUE_MAX_PIXEL_SHARED_REGISTERS 0x400
#define GEN_ROGUE_MAX_COMPUTE_SHARED_REGISTERS 0x800
#define GEN_ROGUE_CSRM_LINE_SIZE_IN_DWORDS 0x400
#define GEN_rogue_fwif_fwccb_cmd_context_reset_data__server_common_context_id 0
#define GEN_rogue_fwif_fwccb_cmd_context_reset_data__reset_reason 4
#define GEN_rogue_fwif_fwccb_cmd_context_reset_data__dm 8
#define GEN_rogue_fwif_fwccb_cmd_context_reset_data__reset_job_ref 12
#define GEN_rogue_fwif_fwccb_cmd_context_reset_data__flags 16
#define GEN_ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_NOTIFICATION 0x2abc0069
#define GEN_ROGUE_FWIF_FWCCB_CMD_FREELISTS_RECONSTRUCTION 0x2abc0068
