add_executable(sdl ${SOURCES_SERVER_SDL})
target_link_libraries(sdl
        X11 z ncurses dl GL pthread asound
        SDL SDL_mixer SDL2_net
        )

if (X86_64)
    add_definitions(-DX86_64)
elseif (X86)
    add_definitions(-DX86)
    set_target_properties(sdl PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
else()
    set_target_properties(sdl PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()