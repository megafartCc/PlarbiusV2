cmake_minimum_required(VERSION 3.24)
project(PlarbiusV2 VERSION 0.1.0 LANGUAGES CXX)

option(PLARBIUS_BUILD_TESTS "Build unit and smoke tests" ON)
option(PLARBIUS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(plarbius_core STATIC
  src/cfr/cfr_plus_trainer.cpp
  src/cfr/infoset_checkpoint_io.cpp
  src/cfr/infoset_table.cpp
  src/cfr/mccfr_trainer.cpp
  src/cfr/regret_matcher.cpp
  src/eval/kuhn_exploitability.cpp
  src/eval/selfplay_evaluator.cpp
  src/game/action.cpp
  src/games/leduc/leduc_game.cpp
  src/games/kuhn/kuhn_game.cpp
  src/infra/logger.cpp
  src/policy/policy_io.cpp
)

target_include_directories(plarbius_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

if(MSVC)
  target_compile_options(plarbius_core PRIVATE /W4 /permissive-)
  if(PLARBIUS_WARNINGS_AS_ERRORS)
    target_compile_options(plarbius_core PRIVATE /WX)
  endif()
else()
  target_compile_options(plarbius_core PRIVATE -Wall -Wextra -Wpedantic)
  if(PLARBIUS_WARNINGS_AS_ERRORS)
    target_compile_options(plarbius_core PRIVATE -Werror)
  endif()
endif()

add_executable(plarbius_train apps/train_blueprint_main.cpp)
target_link_libraries(plarbius_train PRIVATE plarbius_core)

add_executable(plarbius_selfplay apps/selfplay_main.cpp)
target_link_libraries(plarbius_selfplay PRIVATE plarbius_core)

if(PLARBIUS_BUILD_TESTS)
  enable_testing()

  add_executable(test_regret_matcher tests/regret_matcher_test.cpp)
  target_link_libraries(test_regret_matcher PRIVATE plarbius_core)
  add_test(NAME regret_matcher COMMAND test_regret_matcher)

  add_executable(test_kuhn_smoke tests/kuhn_training_smoke_test.cpp)
  target_link_libraries(test_kuhn_smoke PRIVATE plarbius_core)
  add_test(NAME kuhn_smoke COMMAND test_kuhn_smoke)

  add_executable(test_checkpoint_io tests/checkpoint_io_test.cpp)
  target_link_libraries(test_checkpoint_io PRIVATE plarbius_core)
  add_test(NAME checkpoint_io COMMAND test_checkpoint_io)

  add_executable(test_mccfr_smoke tests/mccfr_smoke_test.cpp)
  target_link_libraries(test_mccfr_smoke PRIVATE plarbius_core)
  add_test(NAME mccfr_smoke COMMAND test_mccfr_smoke)

  add_executable(test_kuhn_exploitability tests/kuhn_exploitability_test.cpp)
  target_link_libraries(test_kuhn_exploitability PRIVATE plarbius_core)
  add_test(NAME kuhn_exploitability COMMAND test_kuhn_exploitability)

  add_executable(test_policy_io tests/policy_io_test.cpp)
  target_link_libraries(test_policy_io PRIVATE plarbius_core)
  add_test(NAME policy_io COMMAND test_policy_io)

  add_executable(test_leduc_smoke tests/leduc_smoke_test.cpp)
  target_link_libraries(test_leduc_smoke PRIVATE plarbius_core)
  add_test(NAME leduc_smoke COMMAND test_leduc_smoke)

  add_executable(test_mccfr_importance tests/mccfr_importance_test.cpp)
  target_link_libraries(test_mccfr_importance PRIVATE plarbius_core)
  add_test(NAME mccfr_importance COMMAND test_mccfr_importance)
endif()
