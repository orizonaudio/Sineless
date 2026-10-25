# The licence texts that travel in a plugin bundle's Resources: Sineless's, the embedded fonts', and
# those of the third-party code compiled into the bundle, whose licences ask that their text go
# with the binary. Third-party texts are copied into the build tree under names of their own.
# Sets <out> to the files for <format> (VST3 or AU).
function(orizon_licence_files format out)
    set(licences "${CMAKE_BINARY_DIR}/licences")
    set(files
        ${PROJECT_SOURCE_DIR}/LICENSE
        ${PROJECT_SOURCE_DIR}/ui/src/fonts/OFL-Geist.txt)
    # Both formats: the engine's and the editor's JSON (MIT).
    set(thirdParty "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT=LICENSE-nlohmann-json.txt")
    if(format STREQUAL "VST3")
        list(APPEND thirdParty "${vst3sdk_SOURCE_DIR}/LICENSE.txt=LICENSE-VST3-SDK.txt") # MIT
        if(WIN32)
            # The static WebView2 loader (BSD-3-Clause), with Microsoft's third-party notice.
            list(APPEND thirdParty
                 "${webview2_SOURCE_DIR}/LICENSE.txt=LICENSE-WebView2.txt"
                 "${webview2_SOURCE_DIR}/NOTICE.txt=NOTICE-WebView2.txt")
        endif()
    elseif(format STREQUAL "AU")
        # Apache-2.0; the SDK has no NOTICE file.
        list(APPEND thirdParty "${audiounitsdk_SOURCE_DIR}/LICENSE.txt=LICENSE-AudioUnitSDK.txt")
    else()
        message(FATAL_ERROR "orizon_licence_files: unknown format ${format}")
    endif()
    foreach(entry IN LISTS thirdParty)
        string(REPLACE "=" ";" parts "${entry}")
        list(GET parts 0 source)
        list(GET parts 1 name)
        configure_file("${source}" "${licences}/${name}" COPYONLY)
        list(APPEND files "${licences}/${name}")
    endforeach()
    set(${out} ${files} PARENT_SCOPE)
endfunction()
