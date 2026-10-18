# The version an Audio Unit gives hosts in its Info.plist: 0xMMMMmmbb, with one byte each for the
# minor and patch parts. Hosts rescan, and Logic revalidates, when it changes, so two versions must
# never share a code: a part that does not fit is an error, never an overflow into the next field
# (0.5.256 would read as 0.6.0). Sets <code> to the number, or <error> to why there is none.
function(orizon_au_version_code version code error)
    set(${code} "" PARENT_SCOPE)
    set(${error} "" PARENT_SCOPE)
    if(NOT version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        set(${error} "The Audio Unit's version is not major.minor.patch: ${version}" PARENT_SCOPE)
        return()
    endif()
    set(major ${CMAKE_MATCH_1})
    set(minor ${CMAKE_MATCH_2})
    set(patch ${CMAKE_MATCH_3})
    if(major GREATER 65535 OR minor GREATER 255 OR patch GREATER 255)
        set(${error}
            "The Audio Unit's version has two bytes for major, one each for minor and patch: ${version}"
            PARENT_SCOPE)
        return()
    endif()
    math(EXPR value "(${major} << 16) | (${minor} << 8) | ${patch}")
    set(${code} ${value} PARENT_SCOPE)
endfunction()
