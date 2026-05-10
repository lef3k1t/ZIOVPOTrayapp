set(RPC_IDL "${CMAKE_SOURCE_DIR}/rpc/ZIOVPOTrayRpc.idl")
set(RPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(RPC_HEADER "${RPC_OUTPUT_DIR}/ZIOVPOTrayRpc.h")
set(RPC_CLIENT_STUB "${RPC_OUTPUT_DIR}/ZIOVPOTrayRpc_c.c")
set(RPC_SERVER_STUB "${RPC_OUTPUT_DIR}/ZIOVPOTrayRpc_s.c")

add_custom_command(
  OUTPUT "${RPC_HEADER}" "${RPC_CLIENT_STUB}" "${RPC_SERVER_STUB}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${RPC_OUTPUT_DIR}"
  COMMAND midl /nologo /out "${RPC_OUTPUT_DIR}" "${RPC_IDL}"
  DEPENDS "${RPC_IDL}"
  VERBATIM
)

add_executable(ZIOVPOtrayapp WIN32
  "ZIOVPOtrayapp.cpp"
  "ZIOVPOtrayapp.h"
  "${RPC_HEADER}"
  "${RPC_CLIENT_STUB}"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET ZIOVPOtrayapp PROPERTY CXX_STANDARD 20)
endif()

target_compile_definitions(ZIOVPOtrayapp PRIVATE UNICODE _UNICODE)
target_include_directories(ZIOVPOtrayapp PRIVATE "${RPC_OUTPUT_DIR}")

if (MSVC)
  target_compile_options(ZIOVPOtrayapp PRIVATE /utf-8)
  set_property(TARGET ZIOVPOtrayapp PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(ZIOVPOtrayapp PRIVATE
  advapi32
  rpcrt4
  user32
  shell32
  gdi32
)

install(TARGETS ZIOVPOtrayapp RUNTIME DESTINATION .)
