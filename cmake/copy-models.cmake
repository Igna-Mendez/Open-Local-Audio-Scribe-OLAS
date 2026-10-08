# copy-models.cmake — copy the models directory into the distribution folder.
#
# Called from the `dist` target with MODELS_SRC and DIST_DIR set. A missing
# models/ is a warning, not an error: a source checkout that has not run
# setup.ps1 should still be able to build, it just will not have a runnable
# distribution until the models are fetched.

if(NOT DEFINED MODELS_SRC OR NOT DEFINED DIST_DIR)
    message(FATAL_ERROR "copy-models.cmake needs MODELS_SRC and DIST_DIR")
endif()

if(NOT IS_DIRECTORY "${MODELS_SRC}")
    message(WARNING
        "No models directory at ${MODELS_SRC}. "
        "Run setup.ps1 (or tools\\fetch-streaming-models.ps1) to fetch them, "
        "then re-run the dist target. The zip will not be runnable without them.")
    return()
endif()

file(GLOB model_dirs RELATIVE "${MODELS_SRC}" "${MODELS_SRC}/*")
if(NOT model_dirs)
    message(WARNING "${MODELS_SRC} is empty; run setup.ps1 to fetch models.")
    return()
endif()

file(COPY "${MODELS_SRC}/" DESTINATION "${DIST_DIR}/models")

foreach(d ${model_dirs})
    if(IS_DIRECTORY "${MODELS_SRC}/${d}")
        message(STATUS "bundled model: ${d}")
    endif()
endforeach()
