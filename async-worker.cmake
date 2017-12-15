include_directories("${CMAKE_SOURCE_DIR}/deps/async-worker/")
file(GLOB async_worker_headers ${CMAKE_SOURCE_DIR}/deps/async-worker/async_worker.h)
source_group(deps\\async-worker FILES ${async_worker_headers})
