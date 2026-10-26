#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* opens4ka_runtime_version(void);
int opens4ka_runtime_available(void);
int opens4ka_runtime_available_for_game(const char* runtime_name);
int opens4ka_runtime_start(const char* game_directory);
int opens4ka_runtime_start_with_options(const char* game_directory, int unlock_characters, int ultrawide,
                                        uint32_t aspect_ratio, uint32_t screen_filter, uint32_t anti_aliasing);
int opens4ka_runtime_start_with_options_for_game(const char* game_directory, const char* runtime_name,
                                                 int unlock_characters, int ultrawide, uint32_t aspect_ratio,
                                                 uint32_t screen_filter, uint32_t anti_aliasing);
// 1: the games show PlayStation button prompts, 0: the Xbox ones; for the next start
void opens4ka_runtime_set_playstation_prompts(int on);
// the keyboard's bindings, "action=Key,Key;action=Key"; for the next start
void opens4ka_runtime_set_keys(const char* keys);
int opens4ka_runtime_poll(void);
void opens4ka_runtime_stop(void);
const char* opens4ka_runtime_last_error(void);

#ifdef __cplusplus
}
#endif
