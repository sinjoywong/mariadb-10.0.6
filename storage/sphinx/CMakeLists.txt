ADD_DEFINITIONS(-DMYSQL_SERVER)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-strict-aliasing")
IF(MSVC)
  LINK_LIBRARIES(ws2_32)
ENDIF(MSVC)
MYSQL_ADD_PLUGIN(sphinx  ha_sphinx.cc STORAGE_ENGINE)

IF(NOT WITHOUT_SPHINX_STORAGE_ENGINE)
  ADD_LIBRARY(snippets_udf MODULE  snippets_udf.cc)
  TARGET_LINK_LIBRARIES(snippets_udf mysys) # my_error
  ADD_DEPENDENCIES(snippets_udf GenError) # uses generated error constants
  SET_TARGET_PROPERTIES(snippets_udf PROPERTIES OUTPUT_NAME sphinx PREFIX "")
  INSTALL(TARGETS snippets_udf COMPONENT Server DESTINATION ${INSTALL_PLUGINDIR})
ENDIF()

