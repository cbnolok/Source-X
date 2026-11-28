// Client packets that act on an object, or show it, with the checks of the normal path: the targeted skill (0xBF.0x2E),
//  the pages of a book (0x66), the status of a char (0x34), the design of a custom house (0xBF.0x1E), the unequip macro
//  (0xED).

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestObjects.h"
#include "fixture/CTestPacket.h"
#include "../../src/common/sphereproto.h"
#include "../../src/game/chars/CChar.h"
#include "../../src/game/clients/CClient.h"
#include "../../src/game/items/CItemContainer.h"
#include "../../src/game/items/CItemMessage.h"
#include "../../src/game/items/CItemMultiCustom.h"
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr ITEMID_TYPE kidBook = ITEMID_TYPE(0x0ff0);
    constexpr ITEMID_TYPE kidHouse = ITEMID_TYPE(ITEMID_MULTI + 0x13ec);

    constexpr std::string_view kScript = R"(
[ITEMDEF 0ff0]
DEFNAME=i_fixture_access_book
NAME=fixture book
TYPE=t_book

[MULTIDEF 013ec]
DEFNAME=m_fixture_foundation
TYPE=t_multi_custom
NAME=fixture foundation
MULTIREGION=-3,-3,3,4
)";
    // After the header of the skill section: [SKILL n], n from the enum.
    constexpr std::string_view kSkillScript = R"(
DEFNAME=Skill_EvalInt
KEY=EvaluatingIntel
PROMPT_MSG=Select char to inspect.
DELAY=1.0
)";

    // Where the char of the client is, and points from it on the same line: in reach, in sight but out of reach, out of
    //  sight (of a char or an item, and of a house: its size adds to the range).
    constexpr short kiX = 100, kiY = 100;
    constexpr short kiXInReach = 101;
    constexpr short kiXInSight = 105;
    constexpr short kiXOutOfSight = 140;
    constexpr short kiXHouseOutOfSight = 180;

    void LoadScript()
    {
        std::string sScript(kScript);
        sScript.append("\n[SKILL ").append(std::to_string(int(SKILL_EVALINT))).append("]").append(kSkillScript);
        REQUIRE(CTestEngine::LoadScript("t_packetaccess", sScript));
    }

    // A char of the test, at that point.
    CChar* CreateChar(CTestObjects& objs, short x)
    {
        CChar* pChar = objs.Keep(CChar::CreateBasic(CTestEngine::CharDef(CREID_MAN)));
        REQUIRE(pChar != nullptr);
        REQUIRE(pChar->MoveToChar(CTestEngine::Point(x, kiY)));
        return pChar;
    }

    // 0xBF.0x2E: a skill used on a target, chosen in the client (the skill hotbar of the KR and Enhanced clients).
    std::vector<byte> TargetedSkill(SKILL_TYPE skill, const CObjBase* pTarget)
    {
        return CTestPacket::Variable(XCMD_ExtData).Word(EXTDATA_TargetedSkill).Word(word(skill))
            .Dword(dword(pTarget->GetUID())).Get();
    }

    // 0x66: a page written in a book, with one line.
    std::vector<byte> BookPage(const CItem* pBook, word wPage, const std::string& sLine)
    {
        return CTestPacket::Variable(XCMD_BookPage).Dword(dword(pBook->GetUID())).Word(1).Word(wPage).Word(1)
            .StringNull(sLine).Get();
    }

    // 0x34: a request of the status of an object (type 4: the status bar).
    std::vector<byte> StatusRequest(const CObjBase* pObj)
    {
        return CTestPacket(XCMD_CharStatReq).Dword(0xEDEDEDED).Byte(4).Dword(dword(pObj->GetUID())).Get();
    }

    // 0xBF.0x1E: a request of the design of a custom house.
    std::vector<byte> HouseDesignRequest(const CItem* pHouse)
    {
        return CTestPacket::Variable(XCMD_ExtData).Word(EXTDATA_HouseDesignDet).Dword(dword(pHouse->GetUID())).Get();
    }

    // 0xED: the macro that unequips the items of these layers (at most 3).
    std::vector<byte> UnequipMacro(std::initializer_list<LAYER_TYPE> layers)
    {
        CTestPacket packet = CTestPacket::Variable(XCMD_MacroUnEquipItem);
        packet.Byte(byte(layers.size()));
        for (const LAYER_TYPE layer : layers)
            packet.Word(word(layer));
        return packet.Get();
    }
}

TEST_SUITE("Packet access")
{
    TEST_CASE("Targeted skill (0xBF.0x2E): the target must be in sight, as for a target cursor")
    {
        LoadScript();
        CTestRunModeScope run;
        CTestObjects objs;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(kiX, kiY));
        CClient* pClient = client.GetClient();
        REQUIRE_FALSE(pClient->IsPriv(PRIV_GM));
        REQUIRE(g_Cfg.GetSkillDef(SKILL_EVALINT) != nullptr);

        // The skill button: a target cursor, for the target. 0xBF.0x2E takes the same path, without the cursor.
        client.ClearSent();
        pClient->Event_Skill_Use(SKILL_EVALINT);
        CHECK(pClient->GetTargMode() == CLIMODE_TARG_SKILL);
        CHECK(client.CountSent(XCMD_Target) == 1);

        // Out of sight: before, the skill was started on any char of the world, without the checks of a target cursor.
        const CChar* pFar = CreateChar(objs, kiXOutOfSight);
        REQUIRE_FALSE(pChar->CanSee(pFar));
        client.Receive(TargetedSkill(SKILL_EVALINT, pFar));
        CHECK(pChar->Skill_GetActive() != SKILL_EVALINT);
        CHECK(pChar->m_Act_UID != pFar->GetUID());
        CHECK(pClient->GetTargMode() == CLIMODE_NORMAL);

        // In sight: the skill starts on that target, with no target cursor (the client already chose the target).
        const CChar* pNear = CreateChar(objs, kiXInReach);
        REQUIRE(pChar->CanSee(pNear));
        client.ClearSent();
        client.Receive(TargetedSkill(SKILL_EVALINT, pNear));
        CHECK(pChar->Skill_GetActive() == SKILL_EVALINT);
        CHECK(pChar->m_Act_UID == pNear->GetUID());
        CHECK(pClient->GetTargMode() == CLIMODE_NORMAL);
        CHECK(client.CountSent(XCMD_Target) == 0);
    }

    TEST_CASE("Book pages (0x66): a book in sight but out of reach is read, not written, as for its title")
    {
        LoadScript();
        CTestRunModeScope run;
        CTestObjects objs;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(kiX, kiY));

        CItemMessage* pBook = objs.Keep(dynamic_cast<CItemMessage*>(CItem::CreateScript(kidBook)));
        REQUIRE(pBook != nullptr);
        REQUIRE(pBook->IsBookWritable());
        REQUIRE(pBook->MoveToCheck(CTestEngine::Point(kiXInSight, kiY)));
        REQUIRE(pChar->CanSee(pBook));
        REQUIRE_FALSE(pChar->CanTouch(pBook));

        // Before, it needed only to be in sight (CanSee), while its title needs to be in reach (CanTouch).
        client.Receive(BookPage(pBook, 1, "far"));
        CHECK(pBook->GetPageText(0) == nullptr);

        // In reach.
        REQUIRE(pBook->MoveToCheck(CTestEngine::Point(kiXInReach, kiY)));
        REQUIRE(pChar->CanTouch(pBook));
        client.Receive(BookPage(pBook, 1, "near"));
        REQUIRE(pBook->GetPageText(0) != nullptr);
        CHECK(std::string(pBook->GetPageText(0)) == "near");
    }

    TEST_CASE("Status bar (0x34): only for a char in sight")
    {
        CTestRunModeScope run;
        CTestObjects objs;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(kiX, kiY));

        // Before, the status (name, hits, and for a pet its stats) of any char of the world was sent.
        const CChar* pFar = CreateChar(objs, kiXOutOfSight);
        REQUIRE_FALSE(pChar->CanSee(pFar));
        client.ClearSent();
        client.Receive(StatusRequest(pFar));
        CHECK(client.CountSent(XCMD_Status) == 0);

        const CChar* pNear = CreateChar(objs, kiXInReach);
        REQUIRE(pChar->CanSee(pNear));
        client.ClearSent();
        client.Receive(StatusRequest(pNear));
        CHECK(client.CountSent(XCMD_Status) == 1);
    }

    TEST_CASE("House design (0xBF.0x1E): only for a house in sight")
    {
        LoadScript();
        CTestRunModeScope run;
        CTestObjects objs;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(kiX, kiY));

        CItemMultiCustom* pHouse = objs.Keep(dynamic_cast<CItemMultiCustom*>(CItem::CreateScript(kidHouse)));
        REQUIRE(pHouse != nullptr);

        // Before, the design of any custom house of the world was sent.
        REQUIRE(pHouse->MoveToUpdate(CTestEngine::Point(kiXHouseOutOfSight, kiY)));
        REQUIRE_FALSE(pChar->CanSee(pHouse));
        client.ClearSent();
        client.Receive(HouseDesignRequest(pHouse));
        CHECK(client.CountSent(XCMD_AOSCustomHouse) == 0);

        REQUIRE(pHouse->MoveToUpdate(CTestEngine::Point(kiXInSight, kiY)));
        REQUIRE(pChar->CanSee(pHouse));
        client.ClearSent();
        client.Receive(HouseDesignRequest(pHouse));
        CHECK(client.CountSent(XCMD_AOSCustomHouse) == 1);
    }

    TEST_CASE("Unequip macro (0xED): the layers a player can't drag stay equipped")
    {
        CTestRunModeScope run;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(kiX, kiY));
        CItemContainer* pPack = pChar->GetPackSafe();
        const CItemContainer* pBank = pChar->GetBank();
        REQUIRE(pPack != nullptr);
        REQUIRE(pBank != nullptr);

        // Any layer is asked: ItemPickup refuses those a player can't drag (CanMoveItem), as for a drag (0x07).
        client.Receive(UnequipMacro({LAYER_PACK, LAYER_BANKBOX}));
        CHECK(pChar->LayerFind(LAYER_PACK) == pPack);
        CHECK(pChar->LayerFind(LAYER_BANKBOX) == pBank);

        // The macro is handled: an item being dragged goes back to the pack.
        CItem* pGold = CItem::CreateScript(ITEMID_GOLD_C1);
        REQUIRE(pGold != nullptr);
        pChar->LayerAdd(pGold, LAYER_DRAGGING);
        REQUIRE(pChar->LayerFind(LAYER_DRAGGING) == pGold);
        client.Receive(UnequipMacro({LAYER_DRAGGING}));
        CHECK(pChar->LayerFind(LAYER_DRAGGING) == nullptr);
        CHECK(pGold->GetContainer() == pPack);
    }
}
