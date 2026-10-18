# The Audio Unit's version code (cmake/AudioUnitVersion.cmake): each version a distinct code in
# Apple's layout, and a part that would overflow into the next field refused. Run with
# -DMODULE=<path to AudioUnitVersion.cmake>.
include(${MODULE})

function(expect_code version expected)
    orizon_au_version_code(${version} code error)
    if(NOT code STREQUAL expected OR NOT error STREQUAL "")
        message(SEND_ERROR "${version}: expected ${expected}, got '${code}' (${error})")
    endif()
endfunction()

function(expect_refused version)
    orizon_au_version_code(${version} code error)
    if(NOT code STREQUAL "" OR error STREQUAL "")
        message(SEND_ERROR "${version}: expected no code, got '${code}'")
    endif()
endfunction()

expect_code(0.5.2 1282)          # 0x00000502
expect_code(0.6.0 1536)          # 0x00000600
expect_code(1.0.0 65536)         # 0x00010000
expect_code(0.255.255 65535)     # The largest minor and patch.
expect_code(65535.0.0 4294901760) # The largest major.
expect_refused(0.5.256)          # Would read as 0.6.0.
expect_refused(0.256.0)          # Would read as 1.0.0.
expect_refused(65536.0.0)
expect_refused(1.2)
expect_refused(1.2.3-beta)
