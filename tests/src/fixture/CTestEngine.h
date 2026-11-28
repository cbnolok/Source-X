#ifndef _INC_CTESTENGINE_H
#define _INC_CTESTENGINE_H

#include "../../../src/common/CPointBase.h"
#include "../../../src/game/CServer.h"
#include "../../../src/game/uo_files/uofiles_enums_creid.h"
#include "../../../src/game/uo_files/uofiles_enums_itemid.h"
#include <string_view>

// A minimal engine for the integration tests, set up at the first use and kept until the end of the run:
//  - map 0, flat (terrain kwTerrain, z 0, no statics), from client files generated in a temporary folder: map0.mul,
//    staidx0.mul, statics0.mul, multi.idx and multi.mul (no multis) and tiledata.mul (every item entry is valid, with
//    the flags and layer of the client files for the containers of the chars and the gold, a floor for kidFloorTile
//    and the dirt tile of the house designs, none for the others);
//  - the sectors of the world;
//  - a base script (the skill class of the players, the ITEMDEFs of the containers of the chars and of the gold), and the
//    scripts the tests load (LoadScript): no item can be created without its ITEMDEF;
//  - no region: a char isn't in any (CChar::GetRegion is null);
//  - the names of the triggers;
//  - CLIENTLINGERTIME=0, so the chars of the fake clients log out at once.
// The server mode isn't changed: the objects get a UID only in CTestRunModeScope.
class CTestEngine
{
public:
    static constexpr short kiMapSize = 512;         // Map 0 is 512x512, sectors of 64.
    static constexpr word kwTerrain = 0x0003;       // Grass.
    static constexpr ITEMID_TYPE kidFloorTile = ITEMID_TYPE(0x0519);    // A floor: flat, UFLAG1_FLOOR (cobblestones).

    static void Init();

    // Writes the script text in the temporary folder and loads it as a resource file, once per name: its sections are
    //  defined until the end of the run. False if the load failed.
    static bool LoadScript(const char* pcName, std::string_view svText);

    // The CHARDEF of this ID: the one of a script loaded, or else a bare one, created now.
    static CREID_TYPE CharDef(CREID_TYPE id);

    // A point on map 0, at z 0.
    static CPointMap Point(short x, short y)
    {
        return CPointMap(x, y, 0, 0);
    }
};

// In a startup mode the objects created don't get a UID. In this scope they get one, as in a running server.
struct CTestRunModeScope
{
    const ServMode prevMode = g_Serv.GetServerMode();
    CTestRunModeScope()  { g_Serv.SetServerMode(ServMode::Run); }
    ~CTestRunModeScope() { g_Serv.SetServerMode(prevMode); }
};

#endif // _INC_CTESTENGINE_H
