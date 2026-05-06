add_executable(ZIOVPOtrayapp WIN32
  "ZIOVPOtrayapp.cpp"
  "ZIOVPOtrayapp.h"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET ZIOVPOtrayapp PROPERTY CXX_STANDARD 20)
endif()

target_compile_definitions(ZIOVPOtrayapp PRIVATE UNICODE _UNICODE)

if (MSVC)
  target_compile_options(ZIOVPOtrayapp PRIVATE /utf-8)
endif()

target_link_libraries(ZIOVPOtrayapp PRIVATE
  user32
  shell32
  gdi32
)

install(TARGETS ZIOVPOtrayapp RUNTIME DESTINATION .)
