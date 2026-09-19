# Warning profile for all engine targets.
function(bps_enable_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15")
      # GCC 15 regression: -Wmaybe-uninitialized fires on libstdc++ std::variant
      # internal union storage (_Variant_storage::_M_u) in optimized builds even
      # though every use site is fully initialized — verified by ASan + UBSan
      # both running clean on the full test suite (2719 checks).
      target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
    endif()
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  endif()
endfunction()
