add_executable(ZIOVPOtrayinstaller
  "ZIOVPOtrayinstaller.cpp"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET ZIOVPOtrayinstaller PROPERTY CXX_STANDARD 20)
endif()

target_compile_definitions(ZIOVPOtrayinstaller PRIVATE UNICODE _UNICODE)

if (MSVC)
  target_compile_options(ZIOVPOtrayinstaller PRIVATE /utf-8)
  set_property(TARGET ZIOVPOtrayinstaller PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(ZIOVPOtrayinstaller PRIVATE
  advapi32
  shell32
)

install(TARGETS ZIOVPOtrayinstaller RUNTIME DESTINATION .)
