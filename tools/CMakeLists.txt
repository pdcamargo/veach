add_subdirectory(veach-cli)

# luau-analyze, built from the same pinned Luau as the engine so the type
# checker and the VM always agree. Used by CTest (luau.analyze) and CI.
add_custom_target(veach-luau-analyze ALL DEPENDS Luau.Analyze.CLI)
