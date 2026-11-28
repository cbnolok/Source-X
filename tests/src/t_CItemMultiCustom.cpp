// The design of a custom house (CItemMultiCustom): the components taken out of a design are destroyed (they were
//  leaked: by a backup, a restore, a commit, a reset, a removal), and the operations that remove some while they use
//  others (the components a new one replaces, the pieces of a stair) keep working. The leak check needs LeakSanitizer.
// And the lists of a house (CItemMulti) of its locked down items and secured containers: they follow their items (an item
//  destroyed, released, moved to the moving crate leaves them), a UID not in them changes nothing, and RELEASE changes
//  only a container.

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestObjects.h"
#include "CWorldTestAccess.h"
#include "../../src/common/CScript.h"
#include "../../src/game/items/CItemContainer.h"
#include "../../src/game/items/CItemMultiCustom.h"
#include "../../src/game/CObjBase.h"
#include "../../src/game/CServer.h"
#include <cstdio>
#include <string_view>
#include <vector>

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
    constexpr ITEMID_TYPE kidLockable = ITEMID_TYPE(0x0a2a);  // An item to lock down that doesn't stack (gold in the crate would).

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

[ITEMDEF 0a2a]
DEFNAME=i_fixture_lockable
NAME=stool
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

    // An item of that ID on the first floor of the house, at that point of the design, deleted at the end of the test (from
    //  the ground or the moving crate).
    CItem* CreateAt(CTestObjects& objs, CItemMultiCustom* pHouse, ITEMID_TYPE id, int16 dx, int16 dy)
    {
        CItem* pItem = objs.Keep(CItem::CreateScript(id));
        REQUIRE(pItem != nullptr);
        const CPointMap& ptHouse = pHouse->GetTopPoint();
        REQUIRE(pItem->MoveToUpdate(CPointMap(short(ptHouse.m_x + dx), short(ptHouse.m_y + dy), char(ptHouse.m_z + kiZ), ptHouse.m_map)));
        return pItem;
    }

    // A verb of the house with a UID, as a script (UNLOCKITEM 040001234).
    bool HouseVerb(CItemMultiCustom* pHouse, const char* pcKey, const CUID& uid)
    {
        char szArg[16];
        snprintf(szArg, sizeof(szArg), "0%x", dword(uid));
        CScript s(pcKey, szArg);
        return static_cast<CObjBase*>(pHouse)->r_Verb(s, &g_Serv);     // Private in the house, as the scripts call it.
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

TEST_SUITE("House lockdowns and secured containers")
{
    TEST_CASE("A locked down item destroyed leaves the list of its house")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            CItem* pItem = CItem::CreateScript(kidLockable);     // Not kept: destroyed by the test.
            REQUIRE(pItem != nullptr);
            REQUIRE(pItem->MoveToUpdate(pHouse->GetTopPoint()));
            const CUID uidItem = pItem->GetUID();

            // Before, its TAG named the item itself instead of the house: its destruction didn't unlock it, the house kept
            //  its UID (counted in its storage, and taken for a lockdown by the object that reuses it).
            pHouse->LockItem(uidItem);
            REQUIRE(pHouse->GetLockedItemIndex(uidItem) == 0);
            CHECK(pItem->GetLockDownOfMulti() == pHouse->GetUID());

            pItem->Delete();
            CWorldTestAccess::DestroyScheduledDeletions();
            REQUIRE(uidItem.ItemFind() == nullptr);
            CHECK(pHouse->GetLockedItemIndex(uidItem) < 0);
            CHECK(pHouse->GetLockdownCount() == 0);
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }

    TEST_CASE("UNLOCKITEM and RELEASE of an item that isn't in the list remove nothing from it")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            CItem* pLocked = CreateAt(objs, pHouse, kidLockable, 1, 1);
            CItem* pSecured = CreateAt(objs, pHouse, ITEMID_BACKPACK, 1, 2);
            const CItem* pOtherItem = CreateAt(objs, pHouse, kidLockable, 1, 3);
            const CItem* pOtherContainer = CreateAt(objs, pHouse, ITEMID_BACKPACK, 1, 4);
            pHouse->LockItem(pLocked->GetUID());
            pHouse->Secure(pSecured->GetUID());
            REQUIRE(pHouse->GetLockdownCount() == 2);

            // Before, the removal erased the end of the list: its last item (with an empty list, the size wrapped).
            CHECK(HouseVerb(pHouse, "UNLOCKITEM", pOtherItem->GetUID()));
            CHECK(HouseVerb(pHouse, "RELEASE", pOtherContainer->GetUID()));
            CHECK(pHouse->GetLockedItemIndex(pLocked->GetUID()) == 0);
            CHECK(pHouse->GetSecuredContainerIndex(pSecured->GetUID()) == 0);
            CHECK(pHouse->GetLockdownCount() == 2);

            // And the items of the list are released by them.
            CHECK(HouseVerb(pHouse, "UNLOCKITEM", pLocked->GetUID()));
            CHECK(HouseVerb(pHouse, "RELEASE", pSecured->GetUID()));
            CHECK(pHouse->GetLockdownCount() == 0);
            CHECK_FALSE(pLocked->IsAttr(ATTR_LOCKEDDOWN));
            CHECK_FALSE(pSecured->IsAttr(ATTR_SECURE));
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }

    TEST_CASE("RELEASE of an item that isn't a container leaves it as it is")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            CItem* pItem = CreateAt(objs, pHouse, kidLockable, 1, 1);
            pItem->m_uidLink = pHouse->GetUID();     // Linked to the house, as a door or a sign are.

            // Before, it was released as a container: its link cleared, and the UID of the house written past the members
            //  of a CItem (in its tail padding with GCC on Linux, the heap with MinGW).
            CHECK(HouseVerb(pHouse, "RELEASE", pItem->GetUID()));
            CHECK(pItem->m_uidLink == pHouse->GetUID());
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }

    TEST_CASE("Moved to the moving crate (as at the start of a customization): released, the lists emptied")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            const CObjBase* pCrate = pHouse->GetMovingCrate(false).ItemFind();
            CItem* pLocked = CreateAt(objs, pHouse, kidLockable, 1, 1);
            CItem* pSecured = CreateAt(objs, pHouse, ITEMID_BACKPACK, 1, 2);
            pHouse->LockItem(pLocked->GetUID());
            pHouse->Secure(pSecured->GetUID());

            // In the order of @HouseDesignBegin (ARGN2=1). Before, the transfer of the secured containers emptied the list of
            //  the lockdowns: they stayed locked, out of the list; and the secured list kept the containers moved.
            pHouse->TransferSecuredToMovingCrate();
            pHouse->TransferLockdownsToMovingCrate();
            CHECK(pHouse->GetLockdownCount() == 0);
            CHECK(pLocked->GetContainer() == pCrate);
            CHECK(pSecured->GetContainer() == pCrate);
            CHECK_FALSE(pLocked->IsAttr(ATTR_LOCKEDDOWN));
            CHECK_FALSE(pSecured->IsAttr(ATTR_SECURE));
            CHECK_FALSE(pLocked->GetLockDownOfMulti().IsValidUID());    // Its destruction won't look for the house.
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }

    TEST_CASE("CLEARFLOOR: every locked down item and secured container of the floor goes to the moving crate, released")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            std::vector<CItem*> vLocked, vSecured;
            for (int16 i = 0; i < 3; ++i)
            {
                vLocked.push_back(CreateAt(objs, pHouse, kidLockable, i, 0));
                pHouse->LockItem(vLocked.back()->GetUID());
                vSecured.push_back(CreateAt(objs, pHouse, ITEMID_BACKPACK, i, 1));
                pHouse->Secure(vSecured.back()->GetUID());
            }
            REQUIRE(pHouse->GetLockdownCount() == 6);

            // Before, the lists were walked while each item was removed from them: one in two was skipped (moved to the crate
            //  with the other items of the floor, but still locked or secured, and listed), and the walk read past the end.
            CScript s("CLEARFLOOR", "0");
            CHECK(static_cast<CObjBase*>(pHouse)->r_Verb(s, &g_Serv));
            CHECK(pHouse->GetLockdownCount() == 0);
            const CObjBase* pCrate = pHouse->GetMovingCrate(false).ItemFind();
            for (const CItem* pItem : vLocked)
            {
                CHECK(pItem->GetContainer() == pCrate);
                CHECK_FALSE(pItem->IsAttr(ATTR_LOCKEDDOWN));
            }
            for (const CItem* pItem : vSecured)
            {
                CHECK(pItem->GetContainer() == pCrate);
                CHECK_FALSE(pItem->IsAttr(ATTR_SECURE));
            }
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }

    TEST_CASE("A component placed on a locked down item or a secured container: both go to the moving crate, released")
    {
        CTestEngine::Init();
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        {
            CTestObjects objs;
            CItemMultiCustom* pHouse = CreateHouse(objs);
            CItem* pLocked = CreateAt(objs, pHouse, kidLockable, 2, 2);
            CItem* pSecured = CreateAt(objs, pHouse, ITEMID_BACKPACK, 2, 2);
            pHouse->LockItem(pLocked->GetUID());
            pHouse->Secure(pSecured->GetUID());

            // Before, the secured containers were looked for in the list of the lockdowns found there (emptied): the container
            //  stayed in the wall, secured.
            pHouse->AddItem(nullptr, kidWall, 2, 2, kiZ);
            const CObjBase* pCrate = pHouse->GetMovingCrate(false).ItemFind();
            CHECK(pLocked->GetContainer() == pCrate);
            CHECK(pSecured->GetContainer() == pCrate);
            CHECK_FALSE(pLocked->IsAttr(ATTR_LOCKEDDOWN));
            CHECK_FALSE(pSecured->IsAttr(ATTR_SECURE));
            CHECK(pHouse->GetLockdownCount() == 0);
        }
        CHECK(CObjBase::sm_iCount == uiObjs);
    }
}
