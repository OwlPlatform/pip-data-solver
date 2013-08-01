SET(PipSources
  pip-data-solver.cpp
)

add_executable (pip-data-solver ${PipSources})
target_link_libraries (pip-data-solver owl-common owl-solver pthread)

INSTALL(TARGETS pip-data-solver RUNTIME DESTINATION bin/owl)

