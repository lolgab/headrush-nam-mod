/* mod_payloads_embedded.h -- files the non-NAM mods install on the
 * device, compiled into the binary as byte arrays by tools/bin2c.c (see
 * CMakeLists.txt). Text payloads come straight from mods/<mod>/;
 * mx5_screen_server is cross-compiled into blobs/ alongside the NAM
 * libraries (scripts/build_blobs_native.sh). */
#ifndef NAM_MOD_PAYLOADS_EMBEDDED_H
#define NAM_MOD_PAYLOADS_EMBEDDED_H

/* usb-console */
extern const unsigned char g_mod_setup_usb_console_sh[];
extern const unsigned long g_mod_setup_usb_console_sh_len;
extern const unsigned char g_mod_remove_usb_console_sh[];
extern const unsigned long g_mod_remove_usb_console_sh_len;
extern const unsigned char g_mod_usb_console_service[];
extern const unsigned long g_mod_usb_console_service_len;
extern const unsigned char g_mod_usb_console_shell_service[];
extern const unsigned long g_mod_usb_console_shell_service_len;

/* remote-screen */
extern const unsigned char g_mod_mx5_screen_server[];
extern const unsigned long g_mod_mx5_screen_server_len;
extern const unsigned char g_mod_mx5_screen_service[];
extern const unsigned long g_mod_mx5_screen_service_len;

/* tuner-tempo */
extern const unsigned char g_mod_tuner_mx5_qml[];
extern const unsigned long g_mod_tuner_mx5_qml_len;

#endif
