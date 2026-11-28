// The cache of the map blocks (CWorldCache), set up at the startup for the maps of the ini.

#include <doctest/doctest.h>
#include "CWorldTestAccess.h"
#include "fixture/CTestEngine.h"
#include "../../src/common/CUOInstall.h"
#include "../../src/game/CWorldMap.h"
#include "../../src/game/uo_files/CUOMapList.h"

TEST_SUITE("World cache")
{
    TEST_CASE("A map disabled in the ini (MAPx= with no value) gets no cache, and doesn't stop the startup")
    {
        CTestEngine::Init();    // Map 0 is set up, from the ini of the engine.
        constexpr int kiMap = MAP_SUPPORTED_QTY - 1;

        // CServer::Load sets up the cache after reading the ini, before CUOMapList::Init: then the maps that the ini doesn't
        //  name are still "supported", with no size, and not initialized. That state is set again here, for all the maps
        //  but 0, and the settings of the maps are restored at the end, whatever the result.
        struct Restore
        {
            CWorldTestAccess::MapGeoData saved[MAP_SUPPORTED_QTY];
            Restore()
            {
                for (int i = 0; i < MAP_SUPPORTED_QTY; ++i)
                    saved[i] = CWorldTestAccess::GetMapGeoData(i);
            }
            ~Restore()
            {
                for (int i = 0; i < MAP_SUPPORTED_QTY; ++i)
                    CWorldTestAccess::SetMapGeoData(i, saved[i]);
            }
        } restore;
        for (int i = 1; i < MAP_SUPPORTED_QTY; ++i)
            CWorldTestAccess::SetMapGeoData(i, CWorldTestAccess::MapGeoData::invalid());

        char szArgs[] = "";
        REQUIRE(g_MapList.Load(kiMap, szArgs));
        REQUIRE_FALSE(g_MapList.IsMapSupported(kiMap));

        // Before, the cache asked whether the map was initialized, which asserts that it's supported (Debug, Nightly);
        //  without the assertion (Release), it got a cache for the size of an unset map, 65535x65535 (about 512 MB).
        CHECK_NOTHROW(CWorldTestAccess::InitMapCache());
        CHECK(CWorldTestAccess::HasMapCache(0));
        CHECK_FALSE(CWorldTestAccess::HasMapCache(1));
        CHECK_FALSE(CWorldTestAccess::HasMapCache(kiMap));

        // Then the startup opens the files of the maps (CServerConfig::Load): it asked the same of every map. The files of
        //  map 0 are already open, they're kept.
        VERFILE_TYPE iFailed = VERFILE_QTY;
        CHECK_NOTHROW(iFailed = g_Install.OpenFiles(1ull << VERFILE_MAP));
        CHECK(iFailed == VERFILE_QTY);

        // And a map block of that map, at runtime: none (it asserted too; in Release it would read the missing cache).
        CHECK(CWorldMap::GetMapBlock(CPointMap(8, 8, 0, kiMap)) == nullptr);
        CHECK(CWorldMap::GetMapBlock(CTestEngine::Point(8, 8)) != nullptr);
    }
}
