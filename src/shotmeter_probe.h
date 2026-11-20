#ifndef SHOTMETER_PROBE_H
#define SHOTMETER_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int shotmeter_probe_install(void);
uint32_t shotmeter_probe_get_draw_sample_count(void);
uint32_t shotmeter_probe_get_release_sample_count(void);
const char *shotmeter_probe_get_version(void);

#ifdef __cplusplus
}
#endif

#endif
