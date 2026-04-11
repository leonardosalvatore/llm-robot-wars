#pragma once

#include <stdbool.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "game.h"

void scripting_init(void);

lua_State *scripting_load(const char *path);

const char *scripting_get_last_error(void);

bool scripting_check_syntax_file(const char *path, char *err_buf, int err_size);

void scripting_call_init(lua_State *L, BotConfig *out);

void scripting_shutdown(void);
