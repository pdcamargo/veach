veach_add_engine_library(script
    SOURCES
        src/luau_types.cpp
        src/marshal.cpp
        src/script_asset.cpp
        src/script_runtime.cpp
    PUBLIC_DEPS veach::asset
    PRIVATE_DEPS Luau.VM Luau.Compiler
    PCH <flecs.h> <lua.h> <lualib.h>)
