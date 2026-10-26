/* libhevc's function selector for CPUs without its SIMD (anything but x86-64
 * for now): plain C everywhere. Compiled in place of decoder/<arch>/
 * ihevcd_function_selector.c, so no change to libhevc. */
#include <stddef.h>

#include "ihevc_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ihevc_defs.h"
#include "ihevc_debug.h"
#include "ihevc_structs.h"
#include "ihevc_macros.h"
#include "ihevc_platform_macros.h"
#include "ihevc_cabac_tables.h"
#include "ihevc_disp_mgr.h"
#include "ihevc_buf_mgr.h"
#include "ihevc_dpb_mgr.h"
#include "ihevc_error.h"

#include "ihevcd_defs.h"
#include "ihevcd_function_selector.h"
#include "ihevcd_structs.h"

void
ihevcd_init_function_ptr(void *pv_codec)
{
    codec_t *ps_codec = (codec_t *)pv_codec;

    ihevcd_init_function_ptr_generic(&ps_codec->s_func_selector);
}

void
ihevcd_init_arch(void *pv_codec)
{
    ((codec_t *)pv_codec)->e_processor_arch = ARCH_NA;
}
