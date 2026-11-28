// Client serials used as an object of the kind the packet expects, without checking that they name one: the equip
//  request (0x13).

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestObjects.h"
#include "fixture/CTestPacket.h"
#include "../../src/common/sphereproto.h"
#include "../../src/game/chars/CChar.h"
#include "../../src/game/clients/CClient.h"
#include "../../src/game/items/CItem.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace
{
    // An item a char can wear: equipping it reads the char (CChar::CanEquipLayer).
    constexpr ITEMID_TYPE kidRing = ITEMID_TYPE(0x108a);

    constexpr std::string_view kScript = R"(
[ITEMDEF 0108a]
DEFNAME=i_fixture_ring
NAME=fixture ring
TYPE=t_jewelry
LAYER=8
)";

    // 0x07: the client picks up that item (drags it).
    std::vector<byte> Pickup(const CItem* pItem)
    {
        return CTestPacket(XCMD_ItemPickupReq).Dword(dword(pItem->GetUID())).Word(pItem->GetAmount()).Get();
    }

    // 0x13: the item dragged is dropped on a char (the paperdoll), to equip it.
    std::vector<byte> EquipRequest(const CItem* pItem, LAYER_TYPE layer, dword dwTarget)
    {
        return CTestPacket(XCMD_ItemEquipReq).Dword(dword(pItem->GetUID())).Byte(byte(layer)).Dword(dwTarget).Get();
    }
}

TEST_SUITE("Packet serials")
{
    TEST_CASE("Equip request (0x13): a target that isn't a char is refused, and the item dragged goes back")
    {
        CTestRunModeScope run;
        CTestObjects objs;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(100, 100));
        CClient* pClient = client.GetClient();

        REQUIRE(CTestEngine::LoadScript("t_packetserials", kScript));
        CItem* pItem = objs.Keep(CItem::CreateScript(kidRing));
        CItem* pOther = objs.Keep(CItem::CreateScript(ITEMID_GOLD_C1));
        REQUIRE(pItem != nullptr);
        REQUIRE(pOther != nullptr);
        REQUIRE(pItem->Item_GetDef()->IsTypeEquippable());
        REQUIRE(pItem->Item_GetDef()->GetEquipLayer() == LAYER_RING);
        REQUIRE(pOther->MoveToCheck(CTestEngine::Point(102, 102)));

        // Before, the target was used as a char before it was checked: a crash in every build, for a serial that names
        //  an item, or nothing.
        for (const dword dwTarget : {dword(pOther->GetUID()), dword(0), dword(UINT32_MAX)})
        {
            CAPTURE(dwTarget);
            REQUIRE(pItem->MoveToCheck(CTestEngine::Point(101, 100)));
            pClient->m_tNextPickup = 0;     // The fast loot protection: a pickup each third of a second, in real time.
            client.Receive(Pickup(pItem));
            REQUIRE(pChar->LayerFind(LAYER_DRAGGING) == pItem);
            REQUIRE(pClient->GetTargMode() == CLIMODE_DRAG);

            client.Receive(EquipRequest(pItem, LAYER_RING, dwTarget));
            CHECK(pChar->LayerFind(LAYER_DRAGGING) == nullptr);
            CHECK(pItem->IsTopLevel());
            CHECK(pClient->GetTargMode() == CLIMODE_NORMAL);
        }
    }
}
