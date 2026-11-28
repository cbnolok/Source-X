set(SPHERE_TESTS_SOURCES
    tests/src/t_num_parsing.cpp
    tests/src/t_CPointBase.cpp
    tests/src/t_CUOClientVersion.cpp
    tests/src/t_CCrypto.cpp
    tests/src/t_CTickWheel.cpp
    tests/src/t_CWorldTicker.cpp
    tests/src/t_CItemBase.cpp
    tests/src/t_CResourceRef.cpp
    tests/src/t_CFakeClient.cpp
    tests/src/t_CWorldCache.cpp
    tests/src/fixture/CTestEngine.cpp
    tests/src/fixture/CFakeClient.cpp
)
source_group(tests FILES ${SPHERE_TESTS_SOURCES})

#set(SPHERE_HEADERS ${SPHERE_HEADERS} ${SPHERE_TESTS_HEADERS})
set(SPHERE_SOURCES ${SPHERE_SOURCES} ${SPHERE_TESTS_SOURCES})
