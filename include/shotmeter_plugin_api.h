#ifndef SHOTMETER_PLUGIN_API_H
#define SHOTMETER_PLUGIN_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOTMETER_PLUGIN_API_VERSION 1u

#define SHOTMETER_PLUGIN_CAP_LIVE_ENABLE_DISABLE 0x00000001u
#define SHOTMETER_PLUGIN_CAP_PERSISTENT_SETTINGS  0x00000002u
#define SHOTMETER_PLUGIN_CAP_CUSTOMIZATION_STATE  0x00000004u

#define SHOTMETER_PLUGIN_CATEGORY_DESIGN          1u
#define SHOTMETER_PLUGIN_CATEGORY_AUDIO           2u
#define SHOTMETER_PLUGIN_CATEGORY_GREEN_ANIMATION 3u

#define SHOTMETER_PLUGIN_OK               0
#define SHOTMETER_PLUGIN_ERROR_ARGUMENT  -1
#define SHOTMETER_PLUGIN_ERROR_IO        -2

/*
 * Stable C ABI for the NBA 2K20 POT MOD controller.
 *
 * Selection IDs are opaque to the main game library. The shotmeter plugin
 * owns their meaning and persistence. IDs 0..255 are accepted in API v1.
 *
 * API v1 exposes customization-open state and saved selections. The visual
 * customization renderer/input surface is a separate plugin implementation
 * milestone; opening the state flag alone does not claim that UI is rendered.
 */
uint32_t shotmeter_plugin_get_api_version(void);
uint32_t shotmeter_plugin_get_capabilities(void);
int32_t  shotmeter_plugin_set_enabled(uint32_t enabled);
uint32_t shotmeter_plugin_get_enabled(void);
int32_t  shotmeter_plugin_open_customization(void);
int32_t  shotmeter_plugin_close_customization(void);
uint32_t shotmeter_plugin_is_customization_open(void);
int32_t  shotmeter_plugin_set_selection(uint32_t category, uint32_t selection_id);
int32_t  shotmeter_plugin_get_selection(uint32_t category);
int32_t  shotmeter_plugin_flush_settings(void);
uint32_t shotmeter_plugin_get_init_state(void);

#ifdef __cplusplus
}
#endif

#endif
