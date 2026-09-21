# Each CTest invocation owns its isolated output directory.
file(REMOVE_RECURSE "${OUTPUT}")
execute_process(COMMAND "${MPIEXEC}" "${RANK_FLAG}" "${RANKS}" ${PREFLAGS} "${APP}" ${POSTFLAGS}
  --dim 1 --order 3 --cells 8 --particles 512 --steps 10 --quadrature_points exact
  --replay plain,jacobi,chebyshev --output "${OUTPUT}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "LandauDampingFEM smoke run failed: ${result}")
endif()
file(STRINGS "${OUTPUT}/solves-rank0.csv" rows REGEX "^(initial|step),")
list(LENGTH rows count)
if(NOT count EQUAL 11)
  message(FATAL_ERROR "Expected eleven PIC solves, got ${count}")
endif()
