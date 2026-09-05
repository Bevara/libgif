cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBGIF_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_gif.c
)

SET(LIBGIF_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libgif.a
)

SET(LIBGIF_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libgif
        "${LIBGIF_SRC}"
        "${LIBGIF_LIBS}"
        []
        ""
        "${LIBGIF_INC}"
        ""
        "1")
