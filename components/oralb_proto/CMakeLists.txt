idf_component_register(
    SRCS "oralb_proto.c"
    INCLUDE_DIRS "include"
)

# Host-native regression tests. They need no ESP-IDF:
#   cd components/oralb_proto/test && make
