# Deliberately no REQUIRES and no ESP-IDF headers anywhere in this component:
# it must stay compilable on a development machine so the parser, the rate
# math and the quantile math can be tested against real /metrics captures
# under ASan/UBSan. See test/Makefile.
idf_component_register(
    SRCS
        "prom_value.c"
        "prom_text.c"
        "prom_ident.c"
        "prom_math.c"
    INCLUDE_DIRS "include"
)
