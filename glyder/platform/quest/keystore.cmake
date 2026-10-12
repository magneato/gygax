# Make a debug keystore on first use, so a fresh checkout can build and sideload at once.
# Releases sign with a kept keystore instead (GLYDER_KEYSTORE), so updates install over each other.
if(NOT EXISTS "${KEYSTORE}")
    execute_process(COMMAND "${KEYTOOL}" -genkeypair -keystore "${KEYSTORE}" -storepass "${PASS}" -keypass "${PASS}" -alias glyder
                            -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=glyder debug, O=Gygax"
                    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "keytool could not create ${KEYSTORE}")
    endif()
endif()
