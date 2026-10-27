# Runs markdown-core-server over a framed stdin and checks its exit code and
# the answers it frames (completion plan, section 4.2). The transcripts
# check what each answer says; this checks the process around them.
#
#   cmake -DSERVER=... -DCASE=shutdown|eof|broken -DWORK=... -P serve.cmake

function(frame body out)
    string(LENGTH "${body}" size)
    set(${out}
        "${${out}}Content-Length: ${size}\r\n\r\n${body}"
        PARENT_SCOPE)
endfunction()

set(initialize [=[{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}]=])
set(stream "")
if(CASE STREQUAL "shutdown")
    frame("${initialize}" stream)
    frame([=[{"jsonrpc":"2.0","method":"initialized","params":{}}]=] stream)
    frame([=[{"jsonrpc":"2.0","id":2,"method":"shutdown"}]=] stream)
    frame([=[{"jsonrpc":"2.0","method":"exit"}]=] stream)
    # Nothing after exit is read.
    frame([=[{"jsonrpc":"2.0","id":3,"method":"shutdown"}]=] stream)
    set(expected_code 0)
    set(expected_answers 2)
elseif(CASE STREQUAL "eof")
    frame("${initialize}" stream)
    set(expected_code 1)
    set(expected_answers 1)
elseif(CASE STREQUAL "broken")
    set(stream "Content-Size: 2\r\n\r\n{}")
    set(expected_code 1)
    set(expected_answers 0)
else()
    message(FATAL_ERROR "unknown case ${CASE}")
endif()

set(input "${WORK}/serve-${CASE}.in")
set(output "${WORK}/serve-${CASE}.out")
file(WRITE "${input}" "${stream}")
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
