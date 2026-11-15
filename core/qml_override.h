/* qml_override.h -- make the Pedalboard 2.7 `Evil` run an edited BlockDetails.qml.
 *
 * Evil keeps every QML file twice: the source in its qrc and a qmlcachegen unit that wins at load time. The units
 * are found through a hash keyed by static UTF-16 QStrings ("/Pages/BlockDetails.qml"); changing the last character
 * of a key makes the lookup miss, so Qt compiles the embedded source instead. The qrc is not rebuilt, so the edited
 * source must fit the old zlib slot: it is recompressed (comment and blank lines dropped first if it does not fit)
 * and the tail zero padded. tools/qml_patch.py is the same logic for the dev loop; docs/QML_PAGES.md explains it. */
#ifndef NAM_GUI_QML_OVERRIDE_H
#define NAM_GUI_QML_OVERRIDE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Points the details page's per-pedal UI Loader at `page_url` ("file:///usr/Evil/Resources/pedal_ui.qml") for the
 * editor slot the additive pedals use (UI file name containing "MorningGlory", which has no editor of its own).
 * Modifies `data` IN PLACE. Refuses (false, reason in err) unless the stock BlockDetails.qml entry and its unit key are
 * found exactly once. */
bool nam_qml_override_blockdetails(uint8_t* data, size_t data_len, const char* page_url, char* err, size_t err_size);

/* Replaces the stock Groups/EncoderParamInfo.qml (the DRIVE / LEVEL / TONE tiles of the encoder field) by `qml`, which
 * must fit the stock source's slot (plain, uncompressed entry: padded with spaces) and breaks that file's unit key.
 * Refuses unless the stock entry and key are each found exactly once. */
bool nam_qml_override_encoder_tile(uint8_t* data, size_t data_len, const char* qml, size_t qml_len, char* err,
                                   size_t err_size);

/* The encoder field's tile count is the engine's NumEditParams; for the additive pedals it hides the tiles of the
 * switches the pages replace (NAM: Hi-Cut, Reverb: Gain and Hi-Cut). Same refusal rules as above. */
bool nam_qml_override_encoder_field(uint8_t* data, size_t data_len, char* err, size_t err_size);

/* The Rig page's stomp-box blocks: the Reverb's picture is recoloured blue and an overlay (rig_block.qml: knobs that turn
 * with the live values, the model / algorithm under the name) is loaded for the additive pedals. Same refusal rules. */
bool nam_qml_override_stomp(uint8_t* data, size_t data_len, char* err, size_t err_size);

#endif
