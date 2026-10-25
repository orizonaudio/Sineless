# The VST3 plugin's licences travel in its bundle: Sineless's, the embedded fonts', the logo's in a
# branded build, and those of the third-party code compiled into it, whose licences ask that their
# text go with the binary.
# Run with -DRESOURCES=<the bundle's Resources folder> -DWINDOWS=<ON on Windows>
# -DBRANDED=<SINELESS_BRANDING>.
set(expected
    LICENSE
    OFL-Geist.txt
    LICENSE-nlohmann-json.txt
    LICENSE-VST3-SDK.txt)
if(WINDOWS)
    list(APPEND expected LICENSE-WebView2.txt NOTICE-WebView2.txt)
endif()
# The logo's terms travel with the logo, and only with it.
if(BRANDED)
    list(APPEND expected LICENSE-Orizon-Brand.txt)
elseif(EXISTS "${RESOURCES}/LICENSE-Orizon-Brand.txt")
    message(SEND_ERROR "A build without the brand carries the logo's terms")
endif()
foreach(name IN LISTS expected)
    if(NOT EXISTS "${RESOURCES}/${name}")
        message(SEND_ERROR "Missing from the bundle: ${name}")
    endif()
endforeach()
