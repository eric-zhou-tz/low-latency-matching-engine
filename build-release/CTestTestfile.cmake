# CMake generated Testfile for 
# Source directory: /Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine
# Build directory: /Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
include("/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/order_book_test[1]_include.cmake")
include("/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/exchange_test[1]_include.cmake")
include("/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/parser_tests[1]_include.cmake")
include("/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/golden_replay_tests[1]_include.cmake")
include("/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/order_book_invariant_tests[1]_include.cmake")
include("/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/order_book_regression_tests[1]_include.cmake")
subdirs("_deps/unordered_dense-build")
subdirs("_deps/googletest-build")
subdirs("_deps/googlebenchmark-build")
