# Apple's ALAC decoder for AirPlay 1: github.com/macosforge/alac, codec/ (every source file under the Apache License
# 2.0), plus a C entry (alac_glue.cpp). Changed in ALACDecoder.cpp only: the lines marked "Bitline" refuse a packet
# that claims a longer frame than the stream's or reaches past its own end - the decoder trusts its input otherwise.
# Its byte-order switch knows only x86 as little-endian: the ESP32 is too. Apple's warnings are not ours to fix; the
# glue is
idf_component_register(SRCS "ALACDecoder.cpp" "ALACBitUtilities.c" "EndianPortable.c" "ag_dec.c" "dp_dec.c"
                            "matrix_dec.c" "alac_glue.cpp"
                       INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} PRIVATE TARGET_RT_LITTLE_ENDIAN=1)
set_source_files_properties("ALACDecoder.cpp" "ALACBitUtilities.c" "EndianPortable.c" "ag_dec.c" "dp_dec.c"
                            "matrix_dec.c" PROPERTIES COMPILE_OPTIONS "-w")
# the glue sees Apple's four-character constants through ALACAudioTypes.h
set_source_files_properties("alac_glue.cpp" PROPERTIES COMPILE_OPTIONS "-Wno-multichar")
