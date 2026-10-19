# Wspolny kod slowa kluczowego: cechy log-mel, siec neuronowa (wnioskowanie), detektor.
# Te same pliki kompiluje kwstool (TCC64) na PC.
idf_component_register(
    SRCS "kws_feat.c" "nn.c" "kws_detect.c"
    INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
