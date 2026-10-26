/* libavc's function selector for CPUs without its SIMD (anything but x86-64
 * for now): plain C everywhere. Compiled in place of decoder/<arch>/
 * ih264d_function_selector.c, so no change to libavc. */
#include <stddef.h>

#include "ih264_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ih264_defs.h"
#include "ih264_size_defs.h"
#include "ih264_error.h"
#include "ih264_trans_quant_itrans_iquant.h"
#include "ih264_inter_pred_filters.h"
#include "ih264_deblk_edge_filters.h"

#include "ih264d_structs.h"
#include "ih264d_function_selector.h"

void
ih264d_init_function_ptr(dec_struct_t *ps_codec)
{
    ih264d_init_function_ptr_generic(ps_codec);
}

void
ih264d_init_arch(dec_struct_t *ps_codec)
{
    ps_codec->e_processor_arch = ARCH_NA;
}
