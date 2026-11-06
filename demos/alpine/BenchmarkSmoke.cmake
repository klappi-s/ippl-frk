file(REMOVE_RECURSE "${OUTPUT}")
execute_process(COMMAND "${PYTHON}" "${RUNNER}" --application alpine --executable "${APP}"
  --output "${OUTPUT}" --cells-3d 4 --particles 512 --steps 10 --repeats 1
  --variants cg jacobi cheb8 --ranks "${RANKS}" --launcher "'${MPIEXEC}' ${RANK_FLAG} {ranks}"
  --expected-backend "${BACKEND}" --timeout 30
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Alpine ordinary benchmark smoke failed: ${result}")
endif()
