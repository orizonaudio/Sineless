# The product's names, logo and plugin IDs. Official releases build with them; a build of a
# modified version turns SINELESS_BRANDING off (TRADEMARKS.md), which takes a neutral name and
# placeholder IDs, so it can neither pass as Sineless nor be mistaken for it by a host, and leaves
# the logo (ui/src/brand/, which has terms of its own) and its notice out.
option(SINELESS_BRANDING
       "Build with the Sineless and Orizon Audio names, logo and plugin IDs (official releases)" ON)
if(SINELESS_BRANDING)
    set(SINELESS_BRANDED 1)
    set(SINELESS_PRODUCT_NAME "Sineless")
    set(SINELESS_VENDOR_NAME "Orizon Audio")
    set(SINELESS_BUNDLE_ID "com.orizonaudio.sineless")
    set(SINELESS_AU_SUBTYPE "Snls")
    set(SINELESS_AU_MANUFACTURER "Oriz")
else()
    set(SINELESS_BRANDED 0)
    set(SINELESS_PRODUCT_NAME "Scope")
    set(SINELESS_VENDOR_NAME "Unbranded")
    set(SINELESS_BUNDLE_ID "org.example.scope")
    set(SINELESS_AU_SUBTYPE "Scop")
    set(SINELESS_AU_MANUFACTURER "Unbr")
endif()
set(brandingDir ${CMAKE_BINARY_DIR}/generated/branding)
configure_file(${CMAKE_CURRENT_LIST_DIR}/Branding.h.in ${brandingDir}/Branding.h @ONLY)
add_library(branding INTERFACE)
target_include_directories(branding INTERFACE ${brandingDir})
