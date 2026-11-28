// Values of client packets used as indices or lengths, and client text read up to its end: the first message before the
//  login, the pins of a map (0x56), the lines of a book page (0x66), the tracking menu (0x7D), the chat commands (0xB3).

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestChat.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestPacket.h"
#include "../../src/common/sphere_library/sstringobjs.h"
#include "../../src/common/sphereproto.h"
#include "../../src/game/chars/CChar.h"
#include "../../src/game/chars/CCharNPC.h"
#include "../../src/game/clients/CAccount.h"
#include "../../src/game/clients/CClient.h"
#include "../../src/game/items/CItemContainer.h"
#include "../../src/game/items/CItemMap.h"
#include "../../src/game/items/CItemMessage.h"
#include "../../src/game/CServerConfig.h"
#include "fixture/CTestObjects.h"
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr ITEMID_TYPE kidMap = ITEMID_TYPE(0x14eb);
    constexpr ITEMID_TYPE kidBook = ITEMID_TYPE(0x0fef);

    constexpr std::string_view kScript = R"(
[ITEMDEF 014eb]
DEFNAME=i_fixture_map
NAME=fixture map
TYPE=t_map

[ITEMDEF 0fef]
DEFNAME=i_fixture_book
NAME=fixture book
TYPE=t_book
)";

    // An item of that class in the pack of the char: it's destroyed with the char, at the logout.
    template <class T>
    T* CreateInPack(CChar* pChar, ITEMID_TYPE id)
    {
        REQUIRE(CTestEngine::LoadScript("t_packetbounds", kScript));
        T* pItem = dynamic_cast<T*>(CItem::CreateScript(id));
        REQUIRE(pItem != nullptr);
        pChar->GetPackSafe()->ContentAdd(pItem);
        return pItem;
    }

    // 0x56: an edit of the pins of a map.
    std::vector<byte> MapEdit(const CItemMap* pMap, MAPCMD_TYPE action, byte bPin, word x = 0, word y = 0)
    {
        return CTestPacket(XCMD_MapEdit).Dword(dword(pMap->GetUID())).Byte(byte(action)).Byte(bPin).Word(x).Word(y).Get();
    }

    // 0x66: the pages written in a book, each with its number (from 1) and its lines.
    using BookPage = std::pair<word, std::vector<std::string>>;
    std::vector<byte> BookPages(const CItem* pBook, const std::vector<BookPage>& pages)
    {
        CTestPacket packet = CTestPacket::Variable(XCMD_BookPage);
        packet.Dword(dword(pBook->GetUID())).Word(word(pages.size()));
        for (const BookPage& page : pages)
        {
            packet.Word(page.first).Word(word(page.second.size()));
            for (const std::string& sLine : page.second)
                packet.StringNull(sLine);
        }
        return packet.Get();
    }

    std::string PageText(const CItemMessage* pBook, word wPage)
    {
        lpctstr ptcText = pBook->GetPageText(wPage);
        return (ptcText == nullptr) ? std::string("(none)") : std::string(ptcText);
    }

    // 0x7D: the entry selected in a menu (0 cancels it).
    std::vector<byte> MenuChoice(const CClient* pClient, CLIMODE_TYPE mode, word wSelect)
    {
        return CTestPacket(XCMD_MenuChoice).Dword(dword(pClient->m_tmMenu.m_UID)).Word(word(mode)).Word(wSelect)
            .Word(0).Word(0).Get();
    }

    // The packets sent to the client that have that ASCII text, as it is or in UTF-16 (either byte order).
    size_t CountSentText(const CFakeClient& client, std::string_view svText)
    {
        std::string sBigEndian, sLittleEndian;
        for (const char c : svText)
        {
            sBigEndian += '\0';
            sBigEndian += c;
            sLittleEndian += c;
            sLittleEndian += '\0';
        }
        size_t uiCount = 0;
        for (const std::vector<byte>& vPacket : client.GetSent())
        {
            const std::string_view svPacket(reinterpret_cast<const char*>(vPacket.data()), vPacket.size());
            for (const std::string_view sv : {svText, std::string_view(sBigEndian), std::string_view(sLittleEndian)})
            {
                if (svPacket.find(sv) != std::string_view::npos)
                {
                    ++uiCount;
                    break;
                }
            }
        }
        return uiCount;
    }

    bool SentText(const CFakeClient& client, std::string_view svText)
    {
        return CountSentText(client, svText) > 0;
    }

    // The client logs in and opens the chat window (0xB5), with that chat name.
    void LoginToChat(CFakeClient& client, const char* pcChatName)
    {
        client.Login(CTestEngine::Point(100, 100));
        client.GetAccount()->m_sChatName = pcChatName;
        client.Receive(CTestPacket(XCMD_Chat).String("", 63).Get());
    }

    // Str_GetTemp hands out the slots of a pool of contiguous buffers of THREAD_STRING_LENGTH, in a cycle. This returns the
    //  first slot: the next calls return the ones after it, in order.
    tchar* RewindTempStrings()
    {
        tchar* pPrev = Str_GetTemp();
        for (int i = 0; i < 0x10000; ++i)
        {
            tchar* pCur = Str_GetTemp();
            if (pCur < pPrev)
                return pCur;
            pPrev = pCur;
        }
        FAIL("The temporary strings never cycle");
        return nullptr;
    }
}

TEST_SUITE("Packet bounds")
{
    TEST_CASE("Login: a first message longer than any login message is refused, not copied past the event")
    {
        CFakeClient client;
        // A KR client opens with the seed 0xFFFFFFFF alone, then the server sends the encryption (0xE3) and waits.
        REQUIRE(client.ReceiveRaw({0xFF, 0xFF, 0xFF, 0xFF}));
        REQUIRE(client.GetClient()->GetConnectType() == CONNECT_CRYPT);

        // Before, the message was copied in an event (CEvent) whatever its length: past its end in the Release builds, a
        //  failed assertion in the others.
        const unsigned int uiAsserts = Assert_GetFailures();
        CHECK_FALSE(client.ReceiveRaw(std::vector<byte>(sizeof(CEvent) + 1000, 0x80)));
        CHECK(Assert_GetFailures() == uiAsserts);
    }

    TEST_CASE("Map pins: an insert goes before the pin of that index, or after the last one, never past it")
    {
        CTestRunModeScope run;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(100, 100));
        CItemMap* pMap = CreateInPack<CItemMap>(pChar, kidMap);

        client.Receive(MapEdit(pMap, MAP_ADD, 0, 10, 10));
        client.Receive(MapEdit(pMap, MAP_ADD, 0, 20, 20));
        REQUIRE(pMap->m_Pins.size() == 2);

        // Past the end: before, the pin was inserted out of the bounds of the array (a heap overflow).
        client.Receive(MapEdit(pMap, MAP_INSERT, 3, 30, 30));
        client.Receive(MapEdit(pMap, MAP_INSERT, 255, 30, 30));
        CHECK(pMap->m_Pins.size() == 2);

        client.Receive(MapEdit(pMap, MAP_INSERT, 2, 30, 30));
        client.Receive(MapEdit(pMap, MAP_INSERT, 0, 5, 5));
        REQUIRE(pMap->m_Pins.size() == 4);
        CHECK(pMap->m_Pins[0].m_x == 5);
        CHECK(pMap->m_Pins[1].m_x == 10);
        CHECK(pMap->m_Pins[2].m_x == 20);
        CHECK(pMap->m_Pins[3].m_x == 30);

        // A move or a delete of a pin that doesn't exist.
        client.Receive(MapEdit(pMap, MAP_MOVE, 4, 40, 40));
        client.Receive(MapEdit(pMap, MAP_DELETE, 4));
        CHECK(pMap->m_Pins.size() == 4);
        CHECK(pMap->m_Pins[3].m_x == 30);
    }

    TEST_CASE("Map pins: no more than MAX_PINS, added or inserted")
    {
        CTestRunModeScope run;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(100, 100));
        CItemMap* pMap = CreateInPack<CItemMap>(pChar, kidMap);

        for (int i = 0; i < CItemMap::MAX_PINS + 2; ++i)
            client.Receive(MapEdit(pMap, MAP_ADD, 0, word(i), word(i)));
        CHECK(pMap->m_Pins.size() == size_t(CItemMap::MAX_PINS));

        client.Receive(MapEdit(pMap, MAP_INSERT, 0, 500, 500));
        CHECK(pMap->m_Pins.size() == size_t(CItemMap::MAX_PINS));
        CHECK(pMap->m_Pins[0].m_x == 0);
    }

    TEST_CASE("Book pages: each page gets its lines, separated by tabs")
    {
        CTestRunModeScope run;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(100, 100));
        CItemMessage* pBook = CreateInPack<CItemMessage>(pChar, kidBook);
        REQUIRE(pBook->IsBookWritable());

        client.Receive(BookPages(pBook, {{1, {"first", "second"}}, {3, {"third"}}}));
        CHECK(PageText(pBook, 0) == "first\tsecond");
        CHECK(PageText(pBook, 1) == "(none)");     // Not written.
        CHECK(PageText(pBook, 2) == "third");
    }

    TEST_CASE("Book pages: a page longer than the buffer is cut, nothing is written past it, the pages after it are ignored")
    {
        CTestRunModeScope run;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(100, 100));
        CItemMessage* pBook = CreateInPack<CItemMessage>(pChar, kidBook);
        REQUIRE(pBook->IsBookWritable());

        // The page is read in a temporary string (SCRIPT_MAX_LINE_LEN). Before, once a line filled it, the max length of
        //  the next one wrapped below 0, and that line was written past the buffer, over the next temporary strings (or past
        //  the end of their pool). They're cleared here, to find the line in them if it's written there.
        static_assert(THREAD_STRING_LENGTH == SCRIPT_MAX_LINE_LEN);
        constexpr size_t kuiSlots = 32;
        tchar* pSlots = RewindTempStrings() + THREAD_STRING_LENGTH;
        std::memset(pSlots, 0, kuiSlots * THREAD_STRING_LENGTH);
        const std::string sMark(64, 'Q');

        std::string sFirst;
        std::vector<BookPage> pages;
        SUBCASE("a line too long, then another")
        {
            sFirst = std::string(SCRIPT_MAX_LINE_LEN - 1, 'x');
            pages = {{1, {sFirst + std::string(900, 'Q'), sMark}}, {2, {"next"}}};
        }
        SUBCASE("a line that fills the buffer, then others")
        {
            sFirst = std::string(SCRIPT_MAX_LINE_LEN - 2, 'x');
            pages = {{1, {sFirst, sMark, sMark}}, {2, {"next"}}};
        }
        SUBCASE("lines that fill the buffer, then others")
        {
            // With their separators, they fill it but its last byte (for the terminator).
            const std::string sA(2046, 'x'), sB(SCRIPT_MAX_LINE_LEN - 3 - sA.size(), 'y');
            sFirst = sA + '\t' + sB;
            pages = {{1, {sA, sB, sMark, sMark}}, {2, {"next"}}};
        }
        REQUIRE_FALSE(pages.empty());

        client.Receive(BookPages(pBook, pages));
        const std::string_view svSlots(pSlots, kuiSlots * THREAD_STRING_LENGTH);
        CHECK(svSlots.find(sMark) == std::string_view::npos);
        const std::string sPage = PageText(pBook, 0);
        CHECK(sPage.size() == sFirst.size());
        CHECK((sPage == sFirst));     // Not decomposed: a failure doesn't print the 4 KB of the page.
        CHECK(PageText(pBook, 1) == "(none)");
    }

    TEST_CASE("Tracking menu: an entry past the menu is ignored")
    {
        CTestRunModeScope run;
        CTestObjects objs;
        CFakeClient client;
        CChar* pChar = client.Login(CTestEngine::Point(100, 100));
        CClient* pClient = client.GetClient();

        // An animal to track, on the same tile (the tracking distance depends on the skill).
        CChar* pAnimal = objs.Keep(CChar::CreateBasic(CTestEngine::CharDef(CREID_MAN)));
        REQUIRE(pAnimal != nullptr);
        REQUIRE(pAnimal->SetNPCBrain(NPCBRAIN_ANIMAL));
        REQUIRE(pAnimal->MoveToChar(CTestEngine::Point(100, 100)));

        // The skill button opens the menu of the kinds of creatures, then the one of the creatures of that kind.
        auto OpenMenu = [&]()
        {
            REQUIRE(pClient->Cmd_Skill_Tracking());
            REQUIRE(pClient->GetTargMode() == CLIMODE_MENU_SKILL_TRACK_SETUP);
            client.Receive(MenuChoice(pClient, CLIMODE_MENU_SKILL_TRACK_SETUP, 1));     // Animals.
            REQUIRE(pClient->GetTargMode() == CLIMODE_MENU_SKILL_TRACK);
            REQUIRE(pClient->m_tmMenu.m_Item[1] == dword(pAnimal->GetUID()));
        };

        // Past the array of the entries (64): before, a failed assertion, or in the Release builds the UID to track was read
        //  past the array.
        OpenMenu();
        const unsigned int uiAsserts = Assert_GetFailures();
        client.Receive(MenuChoice(pClient, CLIMODE_MENU_SKILL_TRACK, 0xFFFF));
        CHECK(Assert_GetFailures() == uiAsserts);
        CHECK(pChar->Skill_GetActive() != SKILL_TRACKING);

        // The same for the menu of the kinds (in the Release builds, it was the last kind).
        REQUIRE(pClient->Cmd_Skill_Tracking());
        client.Receive(MenuChoice(pClient, CLIMODE_MENU_SKILL_TRACK_SETUP, 64));
        CHECK(Assert_GetFailures() == uiAsserts);
        CHECK(pClient->GetTargMode() != CLIMODE_MENU_SKILL_TRACK);

        // The entry of the animal.
        OpenMenu();
        client.Receive(MenuChoice(pClient, CLIMODE_MENU_SKILL_TRACK, 1));
        CHECK(pChar->m_Act_UID == pAnimal->GetUID());
    }

    TEST_CASE("Chat: /conf reads nothing past its text, where the text of a command before is, of another client")
    {
        CTestRunModeScope run;
        CTestChatScope chat;
        CFakeClient clientA, clientB;
        LoginToChat(clientA, "FixtureA");
        LoginToChat(clientB, "FixtureB");

        // Before, /conf with no name ("b" alone) read the name past the end of its text, in the buffer where the commands
        //  are converted, and sent it back to the client ("no conference named ..."): the text of the command before.
        clientA.Receive(ChatCommand("aXstalemarker"));
        clientB.ClearSent();
        clientB.Receive(ChatCommand("b"));
        CHECK(clientB.CountSent(0xB2) > 0);   // The answer: no such conference.
        CHECK_FALSE(SentText(clientB, "talemarker"));

        // A creates a channel with a password (/newconf), then tries to join it (/conf) with it.
        clientA.Receive(ChatCommand("cRoom{secret}"));
        REQUIRE(clientA.GetClient()->GetChannel() != nullptr);
        clientA.Receive(ChatCommand("b\"Room\" secret"));

        // With no closing quote, before, the password was read past the end of the text: the one of A, B joined with it.
        clientB.Receive(ChatCommand("b\"Room"));
        CHECK(clientB.GetClient()->GetChannel() == nullptr);

        // With the password.
        clientB.Receive(ChatCommand("b\"Room\" secret"));
        CHECK(clientB.GetClient()->GetChannel() == clientA.GetClient()->GetChannel());
    }

    TEST_CASE("Chat: /msg with no message reads nothing past its text")
    {
        CTestRunModeScope run;
        CTestChatScope chat;
        CFakeClient client;
        LoginToChat(client, "FixtureA");
        client.Receive(ChatCommand("cRoom"));     // /newconf: the client creates the channel and joins it.
        REQUIRE(client.GetClient()->GetChannel() != nullptr);

        // A private message to itself (the sender gets a copy), then one with no message (no space after the name): before,
        //  its message was read past the end of the name, in the buffer where it's split, where the message before is.
        //  Both come in the same data, run one after the other by the network input: nothing in between can write over
        //  that buffer (it's on the stack).
        client.ClearSent();
        std::vector<byte> vData = ChatCommand("eFixtureA stalemarker");
        const std::vector<byte> vNoMessage = ChatCommand("eFixtureA");
        vData.insert(vData.end(), vNoMessage.cbegin(), vNoMessage.cend());
        client.Receive(vData);
        CHECK(CountSentText(client, "stalemarker") == 1);
    }
}
