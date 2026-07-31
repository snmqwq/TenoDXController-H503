#ifndef CONFIG_APP_H
#define CONFIG_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

bool config_app_init(void);
void config_app_task(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_APP_H */
