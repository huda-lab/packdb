if(POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif()

set(HIGHS_SOURCES
    # cuPDLP sources
    highs/pdlp/cupdlp/cupdlp_cs.c
    highs/pdlp/cupdlp/cupdlp_linalg.c
    highs/pdlp/cupdlp/cupdlp_proj.c
    highs/pdlp/cupdlp/cupdlp_restart.c
    highs/pdlp/cupdlp/cupdlp_scaling.c
    highs/pdlp/cupdlp/cupdlp_solver.c
    highs/pdlp/cupdlp/cupdlp_step.c
    highs/pdlp/cupdlp/cupdlp_utils.c

    # BasicLU sources
    highs/ipm/basiclu/basiclu_factorize.c
    highs/ipm/basiclu/basiclu_get_factors.c
    highs/ipm/basiclu/basiclu_initialize.c
    highs/ipm/basiclu/basiclu_object.c
    highs/ipm/basiclu/basiclu_solve_dense.c
    highs/ipm/basiclu/basiclu_solve_for_update.c
    highs/ipm/basiclu/basiclu_solve_sparse.c
    highs/ipm/basiclu/basiclu_update.c
    highs/ipm/basiclu/lu_build_factors.c
    highs/ipm/basiclu/lu_condest.c
    highs/ipm/basiclu/lu_dfs.c
    highs/ipm/basiclu/lu_factorize_bump.c
    highs/ipm/basiclu/lu_file.c
    highs/ipm/basiclu/lu_garbage_perm.c
    highs/ipm/basiclu/lu_initialize.c
    highs/ipm/basiclu/lu_internal.c
    highs/ipm/basiclu/lu_markowitz.c
    highs/ipm/basiclu/lu_matrix_norm.c
    highs/ipm/basiclu/lu_pivot.c
    highs/ipm/basiclu/lu_residual_test.c
    highs/ipm/basiclu/lu_setup_bump.c
    highs/ipm/basiclu/lu_singletons.c
    highs/ipm/basiclu/lu_solve_dense.c
    highs/ipm/basiclu/lu_solve_for_update.c
    highs/ipm/basiclu/lu_solve_sparse.c
    highs/ipm/basiclu/lu_solve_symbolic.c
    highs/ipm/basiclu/lu_solve_triangular.c
    highs/ipm/basiclu/lu_update.c

    # IPX sources
    highs/ipm/ipx/basiclu_kernel.cc
    highs/ipm/ipx/basiclu_wrapper.cc
    highs/ipm/ipx/basis.cc
    highs/ipm/ipx/conjugate_residuals.cc
    highs/ipm/ipx/control.cc
    highs/ipm/ipx/crossover.cc
    highs/ipm/ipx/diagonal_precond.cc
    highs/ipm/ipx/forrest_tomlin.cc
    highs/ipm/ipx/guess_basis.cc
    highs/ipm/ipx/indexed_vector.cc
    highs/ipm/ipx/info.cc
    highs/ipm/ipx/ipm.cc
    highs/ipm/ipx/ipx_c.cc
    highs/ipm/ipx/iterate.cc
    highs/ipm/ipx/kkt_solver_basis.cc
    highs/ipm/ipx/kkt_solver_diag.cc
    highs/ipm/ipx/kkt_solver.cc
    highs/ipm/ipx/linear_operator.cc
    highs/ipm/ipx/lp_solver.cc
    highs/ipm/ipx/lu_factorization.cc
    highs/ipm/ipx/lu_update.cc
    highs/ipm/ipx/maxvolume.cc
    highs/ipm/ipx/model.cc
    highs/ipm/ipx/normal_matrix.cc
    highs/ipm/ipx/sparse_matrix.cc
    highs/ipm/ipx/sparse_utils.cc
    highs/ipm/ipx/splitted_normal_matrix.cc
    highs/ipm/ipx/starting_basis.cc
    highs/ipm/ipx/symbolic_invert.cc
    highs/ipm/ipx/timer.cc
    highs/ipm/ipx/utils.cc

    # Core HiGHS sources
    extern/filereaderlp/reader.cpp
    highs/interfaces/highs_c_api.cpp
    highs/io/Filereader.cpp
    highs/io/FilereaderEms.cpp
    highs/io/FilereaderLp.cpp
    highs/io/FilereaderMps.cpp
    highs/io/HighsIO.cpp
    highs/io/HMpsFF.cpp
    highs/io/HMPSIO.cpp
    highs/io/LoadOptions.cpp
    highs/ipm/IpxWrapper.cpp
    highs/lp_data/Highs.cpp
    highs/lp_data/HighsCallback.cpp
    highs/lp_data/HighsDebug.cpp
    highs/lp_data/HighsIis.cpp
    highs/lp_data/HighsInfo.cpp
    highs/lp_data/HighsInfoDebug.cpp
    highs/lp_data/HighsDeprecated.cpp
    highs/lp_data/HighsInterface.cpp
    highs/lp_data/HighsLp.cpp
    highs/lp_data/HighsLpUtils.cpp
    highs/lp_data/HighsModelUtils.cpp
    highs/lp_data/HighsOptions.cpp
    highs/lp_data/HighsRanging.cpp
    highs/lp_data/HighsSolution.cpp
    highs/lp_data/HighsSolutionDebug.cpp
    highs/lp_data/HighsSolve.cpp
    highs/lp_data/HighsStatus.cpp
    highs/mip/HighsCliqueTable.cpp
    highs/mip/HighsConflictPool.cpp
    highs/mip/HighsCutGeneration.cpp
    highs/mip/HighsCutPool.cpp
    highs/mip/HighsDebugSol.cpp
    highs/mip/HighsDomain.cpp
    highs/mip/HighsDynamicRowMatrix.cpp
    highs/mip/HighsFeasibilityJump.cpp
    highs/mip/HighsGFkSolve.cpp
    highs/mip/HighsImplications.cpp
    highs/mip/HighsLpAggregator.cpp
    highs/mip/HighsLpRelaxation.cpp
    highs/mip/HighsMipAnalysis.cpp
    highs/mip/HighsMipSolver.cpp
    highs/mip/HighsMipSolverData.cpp
    highs/mip/HighsModkSeparator.cpp
    highs/mip/HighsNodeQueue.cpp
    highs/mip/HighsObjectiveFunction.cpp
    highs/mip/HighsPathSeparator.cpp
    highs/mip/HighsPrimalHeuristics.cpp
    highs/mip/HighsPseudocost.cpp
    highs/mip/HighsRedcostFixing.cpp
    highs/mip/HighsSearch.cpp
    highs/mip/HighsSeparation.cpp
    highs/mip/HighsSeparator.cpp
    highs/mip/HighsTableauSeparator.cpp
    highs/mip/HighsTransformedLp.cpp
    highs/model/HighsHessian.cpp
    highs/model/HighsHessianUtils.cpp
    highs/model/HighsModel.cpp
    highs/parallel/HighsTaskExecutor.cpp
    highs/pdlp/CupdlpWrapper.cpp
    highs/presolve/HighsPostsolveStack.cpp
    highs/presolve/HighsSymmetry.cpp
    highs/presolve/HPresolve.cpp
    highs/presolve/HPresolveAnalysis.cpp
    highs/presolve/ICrash.cpp
    highs/presolve/ICrashUtil.cpp
    highs/presolve/ICrashX.cpp
    highs/presolve/PresolveComponent.cpp
    highs/qpsolver/a_asm.cpp
    highs/qpsolver/a_quass.cpp
    highs/qpsolver/basis.cpp
    highs/qpsolver/perturbation.cpp
    highs/qpsolver/quass.cpp
    highs/qpsolver/ratiotest.cpp
    highs/qpsolver/scaling.cpp
    highs/simplex/HEkk.cpp
    highs/simplex/HEkkControl.cpp
    highs/simplex/HEkkDebug.cpp
    highs/simplex/HEkkDual.cpp
    highs/simplex/HEkkDualMulti.cpp
    highs/simplex/HEkkDualRHS.cpp
    highs/simplex/HEkkDualRow.cpp
    highs/simplex/HEkkInterface.cpp
    highs/simplex/HEkkPrimal.cpp
    highs/simplex/HighsSimplexAnalysis.cpp
    highs/simplex/HSimplex.cpp
    highs/simplex/HSimplexDebug.cpp
    highs/simplex/HSimplexNla.cpp
    highs/simplex/HSimplexNlaDebug.cpp
    highs/simplex/HSimplexNlaFreeze.cpp
    highs/simplex/HSimplexNlaProductForm.cpp
    highs/simplex/HSimplexReport.cpp
    highs/test_kkt/KktCh2.cpp
    highs/test_kkt/DevKkt.cpp
    highs/util/HFactor.cpp
    highs/util/HFactorDebug.cpp
    highs/util/HFactorExtend.cpp
    highs/util/HFactorRefactor.cpp
    highs/util/HFactorUtils.cpp
    highs/util/HighsHash.cpp
    highs/util/HighsLinearSumBounds.cpp
    highs/util/HighsMatrixPic.cpp
    highs/util/HighsMatrixUtils.cpp
    highs/util/HighsSort.cpp
    highs/util/HighsSparseMatrix.cpp
    highs/util/HighsUtils.cpp
    highs/util/HSet.cpp
    highs/util/HVectorBase.cpp
    highs/util/stringutil.cpp
)

add_library(duckdb_highs STATIC ${HIGHS_SOURCES})

target_include_directories(duckdb_highs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern/filereaderlp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern/pdqsort>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/interfaces>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/io>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/ipm>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/ipm/ipx>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/ipm/basiclu>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/lp_data>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/mip>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/model>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/parallel>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/pdlp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/pdlp/cupdlp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/presolve>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/qpsolver>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/simplex>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/test_kkt>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/highs/util>
)

set_target_properties(duckdb_highs PROPERTIES EXPORT_NAME duckdb_duckdb_highs)

install(TARGETS duckdb_highs
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

disable_target_warnings(duckdb_highs)
