add_library(ame_llm STATIC
  src/client.cpp
  src/curl_transport.cpp
  src/retry_backoff.cpp
  src/components.cpp
  src/lexicon.cpp
)
target_include_directories(ame_llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Request the system declarations for environ, pipe2 and mkostemp explicitly.
target_compile_definitions(ame_llm PRIVATE _GNU_SOURCE)
target_link_libraries(ame_llm PUBLIC ame_core ame_extractor ame_bench)
