file(GLOB SOURCE_LSMT "*.cpp") 

add_library(lsmt_lib STATIC ${SOURCE_LSMT})  

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
