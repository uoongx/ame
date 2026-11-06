add_executable(ame_demo demo_main.cpp)
target_link_libraries(ame_demo PRIVATE ame_engine)

# Offline summary event format and recall regression.
add_executable(ame_e2e_summary e2e_summary.cpp)
target_link_libraries(ame_e2e_summary PRIVATE ame_engine)

# Offline event forgetting regression.
add_executable(ame_e2e_forget e2e_forget.cpp)
target_link_libraries(ame_e2e_forget PRIVATE ame_engine)
