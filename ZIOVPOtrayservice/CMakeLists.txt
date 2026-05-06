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

add_executable(ZIOVPOtrayservice
  "ZIOVPOtrayservice.cpp"
  "ZIOVPOtrayservice.h"
  "${RPC_HEADER}"
  "${RPC_SERVER_STUB}"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET ZIOVPOtrayservice PROPERTY CXX_STANDARD 20)
endif()

target_compile_definitions(ZIOVPOtrayservice PRIVATE UNICODE _UNICODE)
target_include_directories(ZIOVPOtrayservice PRIVATE "${RPC_OUTPUT_DIR}")

if (MSVC)
  target_compile_options(ZIOVPOtrayservice PRIVATE /utf-8)
endif()

target_link_libraries(ZIOVPOtrayservice PRIVATE
  advapi32
  rpcrt4
  userenv
  wtsapi32
)

install(TARGETS ZIOVPOtrayservice RUNTIME DESTINATION .)
