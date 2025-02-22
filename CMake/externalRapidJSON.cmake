include(ExternalProject)

ExternalProject_Add(
  RapidJSON
  PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/external/RapidJSON
  URL https://github.com/Tencent/rapidjson/archive/v1.1.0.tar.gz
  URL_MD5 badd12c511e081fec6c89c43a7027bce
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
)

ExternalProject_Get_Property(RapidJSON source_dir)
set(RAPIDJSON_INCLUDE_DIR ${source_dir}/include)