# Runs markdown-core-server over a framed stdin and checks its exit code and
# the answers it frames (completion plan, section 4.2). The transcripts
# check what each answer says; this checks the process around them.
#
#   cmake -DSERVER=... -DRUNNER=... -DCASE=shutdown|eof|broken -DWORK=... -P serve.cmake

# CMake writes files in text mode on Windows, so the runner writes the
# input: lsp_runner --stream frames each case's messages byte for byte.
if(CASE STREQUAL "shutdown")
    # initialize, initialized, shutdown, exit, and a shutdown after exit
    # that the server must not read.
    set(expected_code 0)
    set(expected_answers 2)
elseif(CASE STREQUAL "eof")
    # initialize, and then the end of the input.
    set(expected_code 1)
    set(expected_answers 1)
elseif(CASE STREQUAL "broken")
    # A header the protocol does not have, and no Content-Length.
    set(expected_code 1)
    set(expected_answers 0)
else()
    message(FATAL_ERROR "unknown case ${CASE}")
endif()

set(input "${WORK}/serve-${CASE}.in")
set(output "${WORK}/serve-${CASE}.out")
execute_process(COMMAND "${RUNNER}" --stream "${CASE}" "${input}" RESULT_VARIABLE written)
if(NOT written EQUAL 0)
    message(FATAL_ERROR "${CASE}: the input cannot be written")
endif()
execute_process(
    COMMAND "${SERVER}"
    INPUT_FILE "${input}"
    OUTPUT_FILE "${output}"
    RESULT_VARIABLE code)
file(READ "${output}" answers)
string(REGEX MATCHALL "Content-Length: [0-9]+" frames "${answers}")
list(LENGTH frames count)
if(NOT code EQUAL expected_code)
    message(FATAL_ERROR "${CASE}: the server exits with ${code}, not ${expected_code}")
endif()
if(NOT count EQUAL expected_answers)
    message(FATAL_ERROR "${CASE}: the server frames ${count} answers, not ${expected_answers}:\n${answers}")
endif()
if(CASE STREQUAL "shutdown" AND NOT answers MATCHES [=["id":2,"result":null]=])
    message(FATAL_ERROR "${CASE}: shutdown is not answered:\n${answers}")
endif()
message(STATUS "${CASE}: exit ${code}, ${count} answers")
