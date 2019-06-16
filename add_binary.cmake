macro(ADD_BINARY BINARY_INPUT ASM_OUTPUT)

  set(BINARY_INPUT_FILE ${PROJECT_SOURCE_DIR}/${BINARY_INPUT})

  if (NOT EXISTS ${BINARY_INPUT_FILE})
    message (FATAL_ERROR "ADD_BINARY - BINARY_INPUT file '${BINARY_INPUT_FILE}' doesn't exist")
  endif ()

  message (STATUS "ADD_BINARY - BINARY_INPUT: ${BINARY_INPUT_FILE}")

  get_filename_component(BINARY_INPUT_EXT ${BINARY_INPUT_FILE} EXT)
  get_filename_component(BINARY_INPUT_NAME_WE ${BINARY_INPUT_FILE} NAME_WE)
  string(REPLACE "." "" BINARY_INPUT_EXT ${BINARY_INPUT_EXT})

  message(STATUS "ADD_BINARY - BINARY_INPUT name: ${BINARY_INPUT_NAME_WE}")
  message(STATUS "ADD_BINARY - BINARY_INPUT extension: ${BINARY_INPUT_EXT}")

  set (ASM_GENERATED_FILE ${PROJECT_BINARY_DIR}/${BINARY_INPUT_NAME_WE}.asm)
  set(${ASM_OUTPUT} ${ASM_GENERATED_FILE})
  message(STATUS "ADD_BINARY - ASM_OUTPUT: ${ASM_OUTPUT}")
  message(STATUS "ADD_BINARY - ASM_GENERATED_FILE: ${ASM_GENERATED_FILE}")

  file(GENERATE OUTPUT ${ASM_GENERATED_FILE}
  CONTENT
"bits 64
section .rodata

global _binary_src_${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT}_start
global _binary_src_${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT}_end
global _binary_src_${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT}_size

_binary_src_${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT}_start:   incbin \"${BINARY_INPUT_FILE}\"
_binary_src_${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT}_end:
_binary_src_${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT}_size:   dd $-_binary_src_${BINARY_INPUT_NAME_WE}_${BINARY_INPUT_EXT}_start")

endmacro()
