# Rebuilding/upgrading must not overwrite a customer's configuration.
if(NOT EXISTS "${DESTINATION}")
    configure_file("${SOURCE}" "${DESTINATION}" COPYONLY)
endif()
