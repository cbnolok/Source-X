// Client packets received while the client has no char: after the game login, at the char selection. Every handler
//  receives them (the network input doesn't filter them by state): each must refuse them, or work without a char. The
//  chat commands (0xB3) were handled. And a client in a chat channel leaves it when it's destroyed, even with no char.

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestChat.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestObjects.h"
#include "fixture/CTestPacket.h"
#include "../../src/common/assertion.h"
#include "../../src/game/chars/CChar.h"
#include "../../src/game/clients/CAccount.h"
#include "../../src/game/clients/CChat.h"
#include "../../src/game/clients/CChatChannel.h"
#include "../../src/game/clients/CClient.h"
#include "../../src/game/items/CItem.h"
#include "../../src/game/CServer.h"
#include "../../src/game/uo_files/uofiles_enums_creid.h"
#include <cstdint>
#include <vector>

namespace
{
    // A packet of that ID and length (0: variable, of a few bytes). The bytes after the ID (and the length) are the 4 of the
    //  pattern (big endian, as a serial), in a cycle from that shift: with the 4 shifts, a serial at any offset reads it.
    std::vector<byte> PatternPacket(byte bId, int iLength, dword dwPattern, size_t uiShift)
    {
        constexpr int kiVariableLength = 64;
        const bool fVariable = (iLength == 0);
        std::vector<byte> vData(size_t(fVariable ? kiVariableLength : iLength));
        for (size_t i = 0; i < vData.size(); ++i)
            vData[i] = byte(dwPattern >> (8 * (3 - ((i + uiShift) % 4))));
        vData[0] = bId;
        if (fVariable)
        {
            vData[1] = byte(kiVariableLength >> 8);
            vData[2] = byte(kiVariableLength & 0xFF);
        }
        return vData;
    }

    struct CBody
    {
        dword dwPattern;
        size_t uiShift;
    };
}

TEST_SUITE("Packets with no char")
{
    TEST_CASE("Every packet at the char selection: refused, or handled without a char, never a crash or a failed assertion")
    {
        CTestEngine::Init();    // The base script (the gold), before a fake client does it.
        // The bodies of the char creation (0x00, 0x8D, 0xF8: legit at the char selection), for its chars.
        for (const CREID_TYPE id : {CREID_MAN, CREID_WOMAN, CREID_ELFMAN, CREID_ELFWOMAN, CREID_GARGMAN, CREID_GARGWOMAN})
            CTestEngine::CharDef(id);

        CTestRunModeScope run;
        CTestObjects objs;
        const CItem* pItem = objs.Keep(CItem::CreateScript(ITEMID_GOLD_C1));
        CChar* pNPC = objs.Keep(CChar::CreateBasic(CTestEngine::CharDef(CREID_MAN)));
        REQUIRE(pItem != nullptr);
        REQUIRE(pNPC != nullptr);
        REQUIRE(const_cast<CItem*>(pItem)->MoveToCheck(CTestEngine::Point(100, 100)));
        REQUIRE(pNPC->MoveToChar(CTestEngine::Point(101, 100)));

        // The bytes of the packets: all 0 (no object, the first index), all 0xFF (counts and indices at their maximum), the
        //  serial of an item, of a char (the handlers that find the object go further).
        std::vector<CBody> vBodies{{0, 0}, {UINT32_MAX, 0}};
        for (const dword dwUID : {dword(pItem->GetUID()), dword(pNPC->GetUID())})
        {
            for (size_t uiShift = 0; uiShift < 4; ++uiShift)
                vBodies.push_back({dwUID, uiShift});
        }

        int iHandled = 0;
        for (int iId = 0; iId <= 0xFF; ++iId)
        {
            const byte bId = byte(iId);
            int iLength;
            {
                const CFakeClient probe;
                iLength = probe.GetPacketLength(bId);
            }
            if (iLength < 0)
                continue;   // No handler: the network input discards it.
            ++iHandled;

            for (const CBody& body : vBodies)
            {
                CAPTURE(iId);
                CAPTURE(body.dwPattern);
                CAPTURE(body.uiShift);
                CFakeClient client;
                client.LoginAccount();
                REQUIRE(client.GetChar() == nullptr);

                // A handler that uses the char without checking it: a crash, or a failed assertion (caught by the network
                //  input in the builds with the assertions, a crash in the others).
                const unsigned int uiAsserts = Assert_GetFailures();
                bool fThrew = false;
                try
                {
                    client.Receive(PatternPacket(bId, iLength, body.dwPattern, body.uiShift));
                }
                catch (...)
                {
                    fThrew = true;
                }
                CHECK_FALSE(fThrew);
                CHECK(Assert_GetFailures() == uiAsserts);
            }
        }
        CHECK(iHandled > 50);   // The handlers of CPacketManager::registerStandardPackets.
    }

    TEST_CASE("Chat (0xB3): a client with no char can't join or create a channel, none is left with its member")
    {
        CTestRunModeScope run;
        CTestChatScope chat;
        CFakeClient clientA;
        clientA.Login(CTestEngine::Point(100, 100));
        clientA.GetAccount()->m_sChatName = "FixtureA";
        clientA.Receive(ChatCommand("cRoom"));      // /newconf: A creates the channel and joins it.
        CChatChannel* pRoom = clientA.GetClient()->GetChannel();
        REQUIRE(pRoom != nullptr);

        {
            // The account of B used the chat before (its chat name is saved), B is at the char selection.
            CFakeClient clientB;
            clientB.LoginAccount();
            clientB.GetAccount()->m_sChatName = "FixtureB";

            // Before, B joined the channel (the chat button needs a char, the commands didn't). At its destruction the
            //  client quit the chat only with a char: the channel kept the member, and used it after (a message of A).
            clientB.Receive(ChatCommand("b\"Room\""));    // /conf
            REQUIRE(clientB.GetClient()->GetChannel() == nullptr);

            // Before, a failed assertion (CChat::CreateChannel), and with no assertions the channel was created and joined.
            const unsigned int uiAsserts = Assert_GetFailures();
            bool fThrew = false;
            try
            {
                clientB.Receive(ChatCommand("cHall"));
            }
            catch (...)
            {
                fThrew = true;
            }
            CHECK_FALSE(fThrew);
            CHECK(Assert_GetFailures() == uiAsserts);
            CHECK(g_Serv.m_Chats.FindChannel("Hall") == nullptr);
            CHECK(clientB.GetClient()->GetChannel() == nullptr);
        }

        // A talks in the channel, with B gone.
        CHECK(pRoom->m_Members.size() == 1);
        clientA.ClearSent();
        clientA.Receive(ChatCommand("aHello"));
        CHECK(clientA.CountSent(XCMD_ChatReq) > 0);
        CHECK(clientA.GetClient()->GetChannel() == pRoom);
    }

    TEST_CASE("Chat: a client in a channel that loses its char, and stays connected, leaves the channel when it goes")
    {
        CTestRunModeScope run;
        CTestChatScope chat;
        CFakeClient clientA;
        clientA.Login(CTestEngine::Point(100, 100));
        clientA.GetAccount()->m_sChatName = "FixtureA";
        clientA.Receive(ChatCommand("cRoom"));
        CChatChannel* pRoom = clientA.GetClient()->GetChannel();
        REQUIRE(pRoom != nullptr);

        {
            CFakeClient clientB;
            clientB.Login(CTestEngine::Point(101, 100));
            clientB.GetAccount()->m_sChatName = "FixtureB";
            clientB.Receive(ChatCommand("b\"Room\""));
            REQUIRE(clientB.GetClient()->GetChannel() == pRoom);
            REQUIRE(pRoom->m_Members.size() == 2);

            // As when a GM takes control of its char (CClient::Cmd_Control), or another login takes it: the client stays
            //  connected, with no char. CharDisconnect quits the chat only if IsChatActive, never true.
            clientB.GetClient()->CharDisconnect();
            REQUIRE(clientB.GetChar() == nullptr);
        }

        // Before, the client quit the chat at its destruction only with a char: the channel kept the member, destroyed,
        //  and used it for the messages of the others (and when they leave).
        REQUIRE(pRoom->m_Members.size() == 1);
        clientA.ClearSent();
        clientA.Receive(ChatCommand("aHello"));
        CHECK(clientA.CountSent(XCMD_ChatReq) > 0);
    }
}
