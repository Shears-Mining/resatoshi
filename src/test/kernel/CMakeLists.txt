add_executable(test_kernel
    test_kernel.cpp
    recycle_params.cpp
)

target_link_libraries(test_kernel
  PRIVATE
    core_interface
    bitcoinkernel
    Boost::headers
)

add_windows_application_manifest(test_kernel)

add_test(NAME test_kernel COMMAND test_kernel)
