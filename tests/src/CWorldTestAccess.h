#ifndef _INC_CWORLDTESTACCESS_H
#define _INC_CWORLDTESTACCESS_H

#include "../../src/game/CWorld.h"
#include "../../src/game/CWorldGameTime.h"
#include "../../src/game/uo_files/CUOMapList.h"
#include <cstddef>

// What the unit tests drive in g_World (a friend of CWorld, under UNIT_TESTING). Shared by the test files: one definition.
struct CWorldTestAccess
{
    // The time of the world only moves forward between the cases: the timing wheel keeps its current time.
    static void AdvanceTime(int64 iMsecs)
    {
        static int64 s_iTime = 1'000'000'000;
        s_iTime += iMsecs;
        g_World._GameClock.InitTime(s_iTime);
    }

    static void Tick()
    {
        g_World._Ticker.Tick();
    }

    // Objects whose deletion was scheduled (CWorldThread::ScheduleObjDeletion), destroyed by the next garbage collection.
    static size_t CountScheduledDeletions()
    {
        return g_World.m_ObjDelete.GetContentCount();
    }
    static void DestroyScheduledDeletions()
    {
        g_World.m_ObjDelete.ClearContainer(false);
    }

    // What CWorld::GarbageCollection does (a world save too, with SAVEGARBAGECOLLECT), without changing the server mode.
    static void CollectGarbage()
    {
        g_World.GarbageCollection_UIDs();
    }

    // Called in the turn of each char in the periodic ticks, before its tick.
    static void SetCharTurnHook(void (*pfnHook)(CChar*))
    {
        g_World._Ticker._pfnUnitTestCharTurn = pfnHook;
    }

    // The cache of the map blocks, for the maps of g_MapList, as CServer::Load allocates it.
    static void InitMapCache()
    {
        g_World._Cache.Init();
    }
    static bool HasMapCache(int iMap)
    {
        return bool(g_World._Cache._mapBlocks[iMap]);
    }

    // The settings of a map (MAPx= of the ini, then what CUOMapList::Init detects), to restore them after a test changed them.
    using MapGeoData = CUOMapList::MapGeoData;
    static MapGeoData GetMapGeoData(int iMap)
    {
        return g_MapList.m_mapGeoData.maps[iMap];
    }
    static void SetMapGeoData(int iMap, const MapGeoData& data)
    {
        g_MapList.m_mapGeoData.maps[iMap] = data;
    }
};

#endif // _INC_CWORLDTESTACCESS_H
