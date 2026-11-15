/* nam_blobs_embedded.h -- blobs/{libnam_hook.so,libnam_preload.so,
 * trampoline_gonk.bin} compiled directly into the binary as byte arrays
 * (see tools/bin2c.c and the generated blob_*.c sources in
 * CMakeLists.txt). No external files are read at runtime. */
#ifndef NAM_BLOBS_EMBEDDED_H
#define NAM_BLOBS_EMBEDDED_H

extern const unsigned char g_nam_hook_so[];
extern const unsigned long g_nam_hook_so_len;

extern const unsigned char g_nam_preload_so[];
extern const unsigned long g_nam_preload_so_len;

extern const unsigned char g_nam_trampoline_gonk_bin[];
extern const unsigned long g_nam_trampoline_gonk_bin_len;

extern const unsigned char g_pagepedal_bin[];
extern const unsigned long g_pagepedal_bin_len;

/* patch/pagepedal-usb.sh (source file, embedded verbatim) */
extern const unsigned char g_pagepedal_usb_sh[];
extern const unsigned long g_pagepedal_usb_sh_len;

/* patch/ui/ (the .qml files there): the detail pages of the additive pedals (source files, embedded verbatim) */
extern const unsigned char g_ui_pedal_qml[];
extern const unsigned long g_ui_pedal_qml_len;
extern const unsigned char g_ui_nam_qml[];
extern const unsigned long g_ui_nam_qml_len;
extern const unsigned char g_ui_reverb_qml[];
extern const unsigned long g_ui_reverb_qml_len;
extern const unsigned char g_ui_nam_fix_qml[]; /* nam_fix.qml: refreshes the stock model readout on every page */
extern const unsigned long g_ui_nam_fix_qml_len;
extern const unsigned char g_ui_pedal_art_qml[]; /* the pedal drawn on the detail page */
extern const unsigned long g_ui_pedal_art_qml_len;
extern const unsigned char g_ui_rig_block_qml[]; /* the overlay on the Rig page's stomp blocks */
extern const unsigned long g_ui_rig_block_qml_len;
extern const unsigned char g_ui_tile_qml[]; /* EncoderParamInfo.qml with the real parameter names */
extern const unsigned long g_ui_tile_qml_len;

#endif
