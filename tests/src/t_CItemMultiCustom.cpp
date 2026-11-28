// The design of a custom house (CItemMultiCustom): the components taken out of a design are destroyed (they were
//  leaked: by a backup, a restore, a commit, a reset, a removal), and the operations that remove some while they use
//  others (the components a new one replaces, the pieces of a stair) keep working. The leak check needs LeakSanitizer.

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestObjects.h"
#include "../../src/game/items/CItemMultiCustom.h"
#include "../../src/game/CObjBase.h"
#include "../../src/game/CServer.h"
#include <string_view>

#if defined(__SANITIZE_ADDRESS__) && !defined(_MSC_VER)
#   define TEST_LEAK_CHECK 1
#elif defined(__has_feature)
#   if __has_feature(address_sanitizer) && !defined(_MSC_VER)
#       define TEST_LEAK_CHECK 1
#   endif
#endif
#ifdef TEST_LEAK_CHECK
#   include <sanitizer/lsan_interface.h>
#endif

namespace
{
    constexpr ITEMID_TYPE kidHouse = ITEMID_TYPE(ITEMID_MULTI + 0x13ee);
    constexpr ITEMID_TYPE kidWall = ITEMID_TYPE(0x0064);    // Not a floor (a unit high).
    constexpr ITEMID_TYPE kidFloor = CTestEngine::kidFloorTile;

    constexpr std::string_view kScript = R"(
[MULTIDEF 013ee]
DEFNAME=m_fixture_design_foundation
TYPE=t_multi_custom
NAME=fixture design foundation
MULTIREGION=-3,-3,3,4

[ITEMDEF 0e3c]
DEFNAME=i_fixture_moving_crate
TYPE=t_container

[ITEMDEF 064]
DEFNAME=i_fixture_design_wall
NAME=wall

[ITEMDEF 0519]
DEFNAME=i_fixture_design_floor
NAME=floor

[ITEMDEF 031f4]
DEFNAME=i_fixture_design_dirt
NAME=dirt
)";

    // The first floor of the house (plane 1): a client can remove any component there, but the dirt.
    constexpr int8 kiZ = 7;

    // A house, placed, and its moving crate: each component added (out of the loading) creates it, and the house doesn't
    //  delete it (it keeps the items of a house destroyed). Both are deleted at the end of the test.
    CItemMultiCustom* CreateHouse(CTestObjects& objs)
    {
        REQUIRE(CTestEngine::LoadScript("t_citemmulticustom", kScript));
        CItemMultiCustom* pHouse = objs.Keep(dynamic_cast<CItemMultiCustom*>(CItem::CreateScript(kidHouse)));
        REQUIRE(pHouse != nullptr);
        REQUIRE(pHouse->MoveToUpdate(CTestEngine::Point(200, 200)));
        REQUIRE(objs.Keep(pHouse->GetMovingCrate(true).ItemFind()) != nullptr);
        return pHouse;
    }

    // The components at that point (of that ID, or all), once the working design is committed: of the main design, the
    //  one the players see (GetComponentsAt reads it). A commit copies the design too.
    size_t CountAt(CItemMultiCustom* pHouse, int16 x, int16 y, ITEMID_TYPE id = ITEMID_NOTHING)
    {
        pHouse->CommitChanges();
        CItemMultiCustom::CMultiComponent* apComponents[INT8_MAX];
        const size_t uiCount = pHouse->GetComponentsAt(x, y, kiZ, apComponents);
        size_t uiOfId = 0;
        for (size_t i = 0; i < uiCount; ++i)
        {
            if ((id == ITEMID_NOTHING) || (apComponents[i]->m_item.GetDispID() == id))
                ++uiOfId;
        }
        return uiOfId;
    }

    // As the loading of a saved design: the components are added as they are (COMP=), with no replacement.
    struct CLoadingScope
    {
        const ServMode prevMode = g_Serv.GetServerMode();
        CLoadingScope()  { g_Serv.SetServerMode(ServMode::StartupLoadingSaves); }
        ~CLoadingScope() { g_Serv.SetServerMode(prevMode); }
        CLoadingScope(const CLoadingScope&) = delete;
        CLoadingScope& operator=(const CLoadingScope&) = delete;
    };
}

TEST_SUITE("House design")
{
    TEST_CASE("The components taken out of a design are destroyed: backup, restore, revert, reset, removal")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);

            // Each copy of a design (the backup of the working design, its restore) replaced the components of the copy
            //  without destroying them: a cycle of a change and a backup leaked the whole design.
            for (int16 i = 0; i < 20; ++i)
            {
                pHouse->AddItem(nullptr, kidWall, i, 0, kiZ);
                pHouse->BackupStructure();
            }
            REQUIRE(CountAt(pHouse, 19, 0) == 1);

            // A change after the backup, then the restore of the backup: the change is gone.
            pHouse->AddItem(nullptr, kidWall, 0, 1, kiZ);
            REQUIRE(CountAt(pHouse, 0, 1) == 1);
            pHouse->RestoreStructure();
            CHECK(CountAt(pHouse, 0, 1) == 0);
            CHECK(CountAt(pHouse, 19, 0) == 1);

            // A component replaced by another of its kind, then removed.
            pHouse->AddItem(nullptr, kidWall, 0, 0, kiZ);
            CHECK(CountAt(pHouse, 0, 0) == 1);
            pHouse->RemoveItem(nullptr, kidWall, 0, 0, kiZ);
            CHECK(CountAt(pHouse, 0, 0) == 0);

            // The reset to the foundation (no component: the fixture has no multi.mul data).
            pHouse->ResetStructure();
            CHECK(CountAt(pHouse, 19, 0) == 0);

#ifdef TEST_LEAK_CHECK
            // Every component left is in a design: LeakSanitizer finds no block that nothing points to.
            CHECK(__lsan_do_recoverable_leak_check() == 0);
#endif
        }
        CHECK(CObjBase::sm_iCount == uiObjs);   // Nothing left in the world: the house, its crate.
    }

    TEST_CASE("A new component replaces the ones of its kind there, also two of the same ID (from a saved design)")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);

            // A saved design can have two components of the same ID at a point: the load adds them as they are.
            {
                CLoadingScope loading;
                pHouse->AddItem(nullptr, kidWall, 1, 1, kiZ);
                pHouse->AddItem(nullptr, kidWall, 1, 1, kiZ);
                pHouse->AddItem(nullptr, kidFloor, 1, 1, kiZ);
            }
            REQUIRE(CountAt(pHouse, 1, 1, kidWall) == 2);

            // The new wall replaces both (the removal of the first removes the second, of its ID: its values were read after,
            //  from the component removed), and not the floor, of the other kind.
            pHouse->AddItem(nullptr, kidWall, 1, 1, kiZ);
            CHECK(CountAt(pHouse, 1, 1, kidWall) == 1);
            CHECK(CountAt(pHouse, 1, 1, kidFloor) == 1);
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }

    TEST_CASE("A stair removed by a client: all its pieces go, and the dirt replaces its floor pieces on the ground floor")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            CFakeClient client;
            client.Login(CTestEngine::Point(200, 200));

            // The pieces of stair 5 (a floor, two walls) and a wall of no stair.
            constexpr ushort kuiStair = 5;
            pHouse->AddItem(nullptr, kidFloor, 1, 1, kiZ, kuiStair);
            pHouse->AddItem(nullptr, kidWall, 1, 2, kiZ, kuiStair);
            pHouse->AddItem(nullptr, kidWall, 1, 3, kiZ, kuiStair);
            pHouse->AddItem(nullptr, kidWall, 2, 2, kiZ);
            REQUIRE(CountAt(pHouse, 1, 1, kidFloor) == 1);

            // Before, the dirt was added while the pieces were removed, in the list being walked.
            pHouse->RemoveItem(client.GetClient(), kidWall, 1, 2, kiZ);
            CHECK(CountAt(pHouse, 1, 2) == 0);
            CHECK(CountAt(pHouse, 1, 3) == 0);
            CHECK(CountAt(pHouse, 1, 1, kidFloor) == 0);
            CHECK(CountAt(pHouse, 1, 1, ITEMID_DIRT_TILE) == 1);
            CHECK(CountAt(pHouse, 2, 2, kidWall) == 1);

#ifdef TEST_LEAK_CHECK
            CHECK(__lsan_do_recoverable_leak_check() == 0);
#endif
        }
        CHECK(CObjBase::sm_iCount == uiObjs);   // And the char of the client.
    }

    TEST_CASE("A stair removed by a client with no ID (anything at the point): the dirt replacing its floor piece stays")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            CFakeClient client;
            client.Login(CTestEngine::Point(200, 200));

            // Two pieces of the stair at the point: a floor and a wall. The removal finds both, the first takes the whole
            //  stair (both destroyed), and its floor gets a dirt tile. If the dirt was allocated before the removal had
            //  checked the second (a pointer to it, destroyed), the dirt could get its address, and was removed as it.
            constexpr ushort kuiStair = 7;
            pHouse->AddItem(nullptr, kidFloor, 3, 3, kiZ, kuiStair);
            pHouse->AddItem(nullptr, kidWall, 3, 3, kiZ, kuiStair);
            REQUIRE(CountAt(pHouse, 3, 3) == 2);

            pHouse->RemoveItem(client.GetClient(), ITEMID_NOTHING, 3, 3, kiZ);
            CHECK(CountAt(pHouse, 3, 3, kidFloor) == 0);
            CHECK(CountAt(pHouse, 3, 3, kidWall) == 0);
            CHECK(CountAt(pHouse, 3, 3, ITEMID_DIRT_TILE) == 1);
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }
}
