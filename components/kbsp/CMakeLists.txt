# kbsp: the Kode Dot's pins, addresses, geometry and owners; depends on nothing.
idf_component_register(
    SRCS "owners/src/kbsp_owners.c" "exp/src/kbsp_exp.c" "exp/src/kbsp_station.c"
    INCLUDE_DIRS
        "board/include"
        "exp/include"
        "owners/include"
)
