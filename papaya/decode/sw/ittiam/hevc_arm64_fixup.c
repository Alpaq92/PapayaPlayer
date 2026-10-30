/* libhevc's decoder/arm64/ihevcd_fmt_conv_420sp_to_420sp.s defines
 * ihevcd_fmt_conv_420sp_to_420sp_av8 but names the 32-bit _a9q in its .type
 * line; clang's assembler turns that into an undefined _a9q symbol, which a
 * shared library cannot leave unresolved. Nothing calls _a9q on ARMv8 (the
 * selector takes _av8), so it forwards there. Built on 64-bit ARM only. */
#include "ihevc_typedefs.h"
#include "ihevcd_fmt_conv.h"

void
ihevcd_fmt_conv_420sp_to_420sp_a9q(UWORD8 *pu1_y_src, UWORD8 *pu1_uv_src, UWORD8 *pu1_y_dst, UWORD8 *pu1_uv_dst,
                                   WORD32 wd, WORD32 ht, WORD32 src_y_strd, WORD32 src_uv_strd, WORD32 dst_y_strd,
                                   WORD32 dst_uv_strd)
{
    ihevcd_fmt_conv_420sp_to_420sp_av8(pu1_y_src, pu1_uv_src, pu1_y_dst, pu1_uv_dst, wd, ht, src_y_strd, src_uv_strd,
                                       dst_y_strd, dst_uv_strd);
}
