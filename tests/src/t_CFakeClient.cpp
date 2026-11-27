// The fixture of the integration tests: the minimal engine (CTestEngine) and the fake clients (CFakeClient).

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestPacket.h"
#include "CWorldTestAccess.h"
#include "../../src/common/CScript.h"
#include "../../src/common/sphereproto.h"
#include "../../src/game/chars/CChar.h"
#include "../../src/game/clients/CClient.h"
#include "../../src/game/items/CItem.h"
#include "../../src/game/items/CItemContainer.h"
#include "../../src/game/uo_files/CUOMapMeter.h"
#include "../../src/game/CSector.h"
#include "../../src/game/CWorldMap.h"

namespace
{
    constexpr std::string_view kScript = R"(
[ITEMDEF 01f14]
DEFNAME=i_fixture_rune
NAME=fixture rune
TYPE=t_normal

[DIALOG d_fixture]
0,0
resizepic 0 0 5054 200 100
button 10 10 4005 4007 1 0 1

[DIALOG d_fixture BUTTON]
ON=1
SRC.TAG.FIXTURE_BUTTON=<EVAL <SRC.TAG0.FIXTURE_BUTTON> + 1>
)";

    void LoadScript()
    {
        REQUIRE(CTestEngine::LoadScript("t_cfakeclient", kScript));
    }

    // The gump a client was sent last, compressed (from 5.0.0) or not: its object and its context.
    struct SentGump
    {
        dword dwSerial;
        dword dwContext;
    };
    SentGump FindLastGump(const CFakeClient& client)
    {
        const std::vector<byte>* pGump = client.FindLastSent(XCMD_CompressedGumpDialog);
        if (pGump == nullptr)
            pGump = client.FindLastSent(XCMD_GumpDialog);
        REQUIRE(pGump != nullptr);
        CTestPacketReader reader(*pGump, 3);
        const dword dwSerial = reader.Dword();
        return {dwSerial, reader.Dword()};
    }

    std::vector<byte> GumpReply(const SentGump& gump, dword dwButton)
    {
        return CTestPacket::Variable(XCMD_GumpDialogRet)
            .Dword(gump.dwSerial)
            .Dword(gump.dwContext)
            .Dword(dwButton)
            .Dword(0)   // Check boxes.
            .Dword(0)   // Texts.
            .Get();
    }
}

TEST_SUITE("Fixture")
{
    TEST_CASE("The engine: map 0 is flat, with the generated terrain, and has its sectors")
    {
        CTestEngine::Init();
        const CPointMap pt = CTestEngine::Point(100, 200);
        const CUOMapMeter* pMeter = CWorldMap::GetMapMeter(pt);
        REQUIRE(pMeter != nullptr);
        CHECK(pMeter->m_wTerrainIndex == CTestEngine::kwTerrain);
        CHECK(pMeter->m_z == 0);
        CHECK(pt.GetSector() != nullptr);
        CHECK(CWorldMap::GetMapMeter(CTestEngine::Point(CTestEngine::kiMapSize - 1, CTestEngine::kiMapSize - 1)) != nullptr);
    }

    TEST_CASE("The engine: the ITEMDEF of a script loaded")
    {
        LoadScript();
        CTestRunModeScope run;
        CItem* pItem = CItem::CreateScript(ITEMID_TYPE(0x1f14));
        REQUIRE(pItem != nullptr);
        CHECK(pItem->GetUID().IsValidUID());
        CHECK(strcmp(pItem->GetName(), "fixture rune") == 0);
        delete pItem;
    }

    TEST_CASE("A fake client: its char enters the world and the client is started")
    {
        CTestRunModeScope run;
        const dword uiObjs = CObjBase::sm_iCount;
        const size_t uiScheduled = CWorldTestAccess::CountScheduledDeletions();
        {
            CFakeClient client;
            const CPointMap pt = CTestEngine::Point(100, 100);
            CChar* pChar = client.Login(pt);
            REQUIRE(pChar != nullptr);
            CHECK(pChar->GetTopPoint() == pt);
            CHECK(pChar->GetClientActive() == client.GetClient());
            CHECK(pChar->m_pPlayer != nullptr);
            // The ITEMDEFs of the base script and the layers of the tiledata: the containers of a char are created and equipped.
            const CItemContainer* pPack = pChar->GetPackSafe();
            REQUIRE(pPack != nullptr);
            CHECK(pPack->GetEquipLayer() == LAYER_PACK);
            CHECK(pChar->LayerFind(LAYER_PACK) == pPack);
            const CItemContainer* pBank = pChar->GetBank();
            REQUIRE(pBank != nullptr);
            CHECK(pBank->GetEquipLayer() == LAYER_BANKBOX);
            CHECK(client.CountSent(XCMD_Start) == 1);
        }
        // The fake client destroys its char with what it carries: nothing is left for the next tests.
        CHECK(CObjBase::sm_iCount == uiObjs);
        CHECK(CWorldTestAccess::CountScheduledDeletions() == uiScheduled);
    }

    TEST_CASE("Two fake clients: the char of each is sent to the other")
    {
        CTestRunModeScope run;
        CFakeClient client1, client2;
        CChar* pChar1 = client1.Login(CTestEngine::Point(100, 100));
        client1.ClearSent();
        CChar* pChar2 = client2.Login(CTestEngine::Point(102, 100));

        // The char that enters is sent to the clients that see it, and the chars it sees are sent to its client.
        auto IsCharSent = [](const CFakeClient& client, const CChar* pChar) {
            for (const std::vector<byte>& vPacket : client.GetSent())
            {
                if ((vPacket[0] == XCMD_Char) && (CTestPacketReader(vPacket, 3).Dword() == dword(pChar->GetUID())))
                    return true;
            }
            return false;
        };
        CHECK(IsCharSent(client1, pChar2));
        CHECK(IsCharSent(client2, pChar1));
    }

    TEST_CASE("A fake client: a dialog sent, then its button replied")
    {
        LoadScript();
        CTestRunModeScope run;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(100, 100));

        CScript s("DIALOG", "d_fixture");
        REQUIRE(pChar->r_Verb(s, pChar));
        const SentGump gump = FindLastGump(client);
        CHECK(gump.dwSerial == dword(pChar->GetUID()));

        client.Receive(GumpReply(gump, 1));
        CHECK(pChar->m_TagDefs.GetKeyNum("FIXTURE_BUTTON") == 1);

        // The dialog was closed by the reply: another reply is rejected.
        client.Receive(GumpReply(gump, 1));
        CHECK(pChar->m_TagDefs.GetKeyNum("FIXTURE_BUTTON") == 1);
    }
}
