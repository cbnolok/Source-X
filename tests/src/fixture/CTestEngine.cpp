#include "CTestEngine.h"
#include "../CWorldTestAccess.h"
#include "../../../src/common/sphere_library/sstring.h"
#include "../../../src/common/CUOInstall.h"
#include "../../../src/game/uo_files/CUOIndexRec.h"
#include "../../../src/game/uo_files/CUOItemTypeRec.h"
#include "../../../src/game/uo_files/CUOMapBlock.h"
#include "../../../src/game/uo_files/CUOMapList.h"
#include "../../../src/game/uo_files/CUOTerrainTypeRec.h"
#include "../../../src/game/uo_files/uofiles_enums.h"
#include "../../../src/game/uo_files/uofiles_enums_itemid.h"
#include "../../../src/game/uo_files/uofiles_macros.h"
#include "../../../src/game/chars/CCharBase.h"
#include "../../../src/game/CServerConfig.h"
#include "../../../src/game/CWorld.h"
#include "../../../src/game/triggers.h"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#   include <process.h>
#   define TEST_GETPID _getpid
#else
#   include <unistd.h>
#   define TEST_GETPID getpid
#endif

namespace
{
    namespace fs = std::filesystem;

    constexpr int kiBlocks = (CTestEngine::kiMapSize / UO_BLOCK_SIZE) * (CTestEngine::kiMapSize / UO_BLOCK_SIZE);
    // The item entries in tiledata.mul: every item ID below ITEMID_MULTI. In the original format, the file stays shorter
    //  than one of the High Seas format.
    constexpr uint kuiTiledataItems = ITEMID_MULTI;

    // The client files the engine opens, without verdata.mul (the map files of map 0).
    constexpr VERFILE_TYPE kOpenedFiles[] = {VERFILE_MAP, VERFILE_STAIDX, VERFILE_STATICS, VERFILE_TILEDATA, VERFILE_MULTIIDX, VERFILE_MULTI};

    // The tiledata entries of the items the server creates by itself, as in the client files: the containers it equips,
    //  the gold (stackable; weightless, as the 0.02 stones of a coin round to 0), and two floor tiles of the house designs
    //  (flat): a floor, the dirt of the ground floor. The others weigh a stone and are a unit high.
    struct TiledataItem
    {
        ITEMID_TYPE id;
        dword dwFlags;
        LAYER_TYPE layer;
        byte uiWeight;
        byte uiHeight;
    };
    constexpr TiledataItem kItems[] = {
        {ITEMID_BACKPACK, UFLAG1_EQUIP | UFLAG3_CONTAINER, LAYER_PACK, 1, 1},
        {ITEMID_BANK_BOX, UFLAG1_EQUIP | UFLAG3_CONTAINER, LAYER_BANKBOX, 1, 1},
        {ITEMID_VENDOR_BOX, UFLAG1_EQUIP | UFLAG3_CONTAINER, LAYER_VENDOR_STOCK, 1, 1},
        {ITEMID_GOLD_C1, UFLAG2_STACKABLE, LAYER_NONE, 0, 1},
        {CTestEngine::kidFloorTile, UFLAG1_FLOOR, LAYER_NONE, 1, 0},
        {ITEMID_DIRT_TILE, UFLAG1_FLOOR, LAYER_NONE, 1, 0},
    };

    // The definitions the server needs for any char: the skill class of the players, the containers and the gold, with
    //  the type names they use.
    constexpr std::string_view kBaseScript = R"(
[SKILLCLASS 0]
DEFNAME=Class_Undeclared
NAME=undeclared
STATSUM=300
SKILLSUM=10000.0
STR=100
INT=100
DEX=100

[TYPEDEFS]
t_normal 0
t_container 1
t_gold 72
t_eq_vendor_box 113
t_eq_bank_box 114

[ITEMDEF 0e75]
DEFNAME=i_backpack
TYPE=t_container

[ITEMDEF 09b2]
DEFNAME=i_bankbox
TYPE=t_eq_bank_box

[ITEMDEF 02af8]
DEFNAME=i_vendor_box
TYPE=t_eq_vendor_box

[ITEMDEF 0eed]
DEFNAME=i_gold
TYPE=t_gold
)";

    bool s_fInitialized = false;
    fs::path s_pathDir;
    std::set<std::string> s_setScriptsLoaded;

    template <typename T>
    void WriteRecord(std::ofstream& file, const T& rec)
    {
        file.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    }

    void WriteHeader(std::ofstream& file)
    {
        const dword dwHeader = 0;   // Each block of 32 tiledata entries starts with an unused header.
        WriteRecord(file, dwHeader);
    }

    void WriteClientFiles()
    {
        // map0.mul: the blocks of 8x8 meters, flat.
        {
            std::ofstream file(s_pathDir / "map0.mul", std::ios::binary | std::ios::trunc);
            CUOMapBlock block{};
            for (CUOMapMeter& meter : block.m_Meter)
            {
                meter.m_wTerrainIndex = CTestEngine::kwTerrain;
                meter.m_z = 0;
            }
            for (int i = 0; i < kiBlocks; ++i)
                WriteRecord(file, block);
        }

        // staidx0.mul and statics0.mul: no statics in any block.
        {
            std::ofstream file(s_pathDir / "staidx0.mul", std::ios::binary | std::ios::trunc);
            std::array<byte, sizeof(CUOIndexRec)> index;
            index.fill(0xFF);       // Offset 0xFFFFFFFF: nothing here.
            for (int i = 0; i < kiBlocks; ++i)
                WriteRecord(file, index);
        }
        {
            std::ofstream file(s_pathDir / "statics0.mul", std::ios::binary | std::ios::trunc);
        }

        // multi.idx and multi.mul: no multis (multi.idx has 8182 entries in the client files).
        {
            std::ofstream file(s_pathDir / "multi.idx", std::ios::binary | std::ios::trunc);
            std::array<byte, sizeof(CUOIndexRec)> index;
            index.fill(0xFF);
            for (int i = 0; i < 8182; ++i)
                WriteRecord(file, index);
        }
        {
            std::ofstream file(s_pathDir / "multi.mul", std::ios::binary | std::ios::trunc);
        }

        // tiledata.mul, original format: the terrain entries, then the item entries. An item entry with no data is
        //  "unused" (CItemBase::GetItemData): each has a weight and a name.
        {
            std::ofstream file(s_pathDir / "tiledata.mul", std::ios::binary | std::ios::trunc);
            for (uint id = 0; id < TERRAIN_QTY; ++id)
            {
                if ((id % UOTILE_BLOCK_QTY) == 0)
                    WriteHeader(file);
                CUOTerrainTypeRec rec{};
                if (id == CTestEngine::kwTerrain)
                    Str_CopyLimitNull(rec.m_name, "grass", sizeof(rec.m_name));
                WriteRecord(file, rec);
            }
            for (uint id = 0; id < kuiTiledataItems; ++id)
            {
                if ((id % UOTILE_BLOCK_QTY) == 0)
                    WriteHeader(file);
                CUOItemTypeRec rec{};
                rec.m_weight = 1;
                rec.m_height = 1;
                Str_CopyLimitNull(rec.m_name, "item", sizeof(rec.m_name));
                for (const TiledataItem& item : kItems)
                {
                    if (item.id == ITEMID_TYPE(id))
                    {
                        rec.m_flags = item.dwFlags;
                        rec.m_layer = byte(item.layer);
                        rec.m_weight = item.uiWeight;
                        rec.m_height = item.uiHeight;
                    }
                }
                WriteRecord(file, rec);
            }
        }
    }

    // At the exit: the client files are closed (Windows doesn't remove open files), then the folder is removed.
    // Only those the engine opened: CUOInstall::CloseFiles locks the mutex of each of its ~2000 files, and with
    //  winpthreads (MinGW) a std::shared_mutex locked the first time creates two semaphores (under Wine, two descriptors).
    void Shutdown()
    {
        for (const VERFILE_TYPE iFile : kOpenedFiles)
            g_Install.m_File[iFile].Close();
        g_Install.m_Maps[0].Close();
        g_Install.m_Staidx[0].Close();
        g_Install.m_Statics[0].Close();
        std::error_code ec;
        fs::remove_all(s_pathDir, ec);
    }
}

void CTestEngine::Init()
{
    if (s_fInitialized)
        return;
    s_fInitialized = true;

    s_pathDir = fs::temp_directory_path() / ("spheretest_engine_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(s_pathDir, ec);
    fs::create_directories(s_pathDir);
    WriteClientFiles();

    // MAP0=512,512,64,0,0: the other maps stay disabled.
    std::string sMapArgs = std::to_string(kiMapSize) + "," + std::to_string(kiMapSize) + ",64,0,0";
    g_MapList.Load(0, sMapArgs.data());

    // As CServer::Load: the cache for the maps of the MAPx lines, before the files are opened (that disables the others).
    CWorldTestAccess::InitMapCache();

    // The client files, from the folder: it caches tiledata.mul and initializes the maps.
    g_Install.SetPreferPath(s_pathDir.string().c_str());
    ullong ullFiles = 0;
    for (const VERFILE_TYPE iFile : kOpenedFiles)
        ullFiles |= (1ull << iFile);
    const VERFILE_TYPE iFailed = g_Install.OpenFiles(ullFiles);
    if (iFailed != VERFILE_QTY)
        throw std::runtime_error(std::string("CTestEngine: client file not opened: ") + g_Install.GetBaseFileName(iFailed));

    g_World.Init();     // The sectors.
    TriglistInit();     // The names of the triggers, for the scripts.

    g_Cfg.m_iClientLingerTime = 0;

    if (!LoadScript("fixture_base", kBaseScript))
        throw std::runtime_error("CTestEngine: the base script wasn't loaded");

    std::atexit(Shutdown);
}

bool CTestEngine::LoadScript(const char* pcName, std::string_view svText)
{
    Init();
    if (!s_setScriptsLoaded.insert(pcName).second)
        return true;

    const fs::path path = s_pathDir / (std::string(pcName) + ".scp");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(svText.data(), std::streamsize(svText.size()));
        file << "\n[EOF]\n";
    }
    return g_Cfg.LoadResourcesAdd(path.string().c_str()) != nullptr;
}

CREID_TYPE CTestEngine::CharDef(CREID_TYPE id)
{
    const CResourceID rid(RES_CHARDEF, id);
    if (g_Cfg.m_ResHash.FindKey(rid) == sl::scont_bad_index())
        g_Cfg.m_ResHash.AddSortKey(rid, new CCharBase(id));
    return id;
}
