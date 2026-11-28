// Menus (0x7C sent, 0x7D replied): the line a client selects runs the entry the server showed there, and a line past
//  the entries sent runs nothing. A script menu doesn't show an entry with a bad item ID: the select counted it (the line
//  after ran the entry before the one shown, the last line ran one never shown). And the lines past the entries sent
//  found those of an earlier menu (with tracking, the UIDs of its targets).

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestPacket.h"
#include "../../src/common/CScript.h"
#include "../../src/common/sphereproto.h"
#include "../../src/game/chars/CChar.h"
#include <string_view>
#include <vector>

namespace
{
    // The second entry has an item ID that isn't defined: not shown. Each entry sets the TAG, as the cancel does.
    constexpr std::string_view kScript = R"(
[MENU m_fixture_menu]
fixture menu
ON=0 first
SRC.TAG.FIXTURE_MENU=1
ON=i_fixture_not_defined second
SRC.TAG.FIXTURE_MENU=2
ON=0 third
SRC.TAG.FIXTURE_MENU=3
ON=@cancel
SRC.TAG.FIXTURE_MENU=-1

[MENU m_fixture_menu_long]
fixture long menu
ON=0 one
SRC.TAG.FIXTURE_MENU=11
ON=0 two
SRC.TAG.FIXTURE_MENU=12
ON=0 three
SRC.TAG.FIXTURE_MENU=13
)";

    // The menu a client was sent last: its object, its context and the count of its entries.
    struct SentMenu
    {
        dword dwSerial;
        word wContext;
        byte bCount;
    };

    SentMenu OpenMenu(CFakeClient& client, const char* pcMenu)
    {
        CChar* pChar = client.GetChar();
        CScript s("MENU", pcMenu);
        REQUIRE(pChar->r_Verb(s, pChar));
        const std::vector<byte>* pMenu = client.FindLastSent(XCMD_MenuItems);
        REQUIRE(pMenu != nullptr);
        CTestPacketReader reader(*pMenu, 3);
        const dword dwSerial = reader.Dword();
        const word wContext = reader.Word();
        reader.Skip(reader.Byte());     // The title.
        return {dwSerial, wContext, reader.Byte()};
    }

    void SelectLine(CFakeClient& client, const SentMenu& menu, word wLine)
    {
        client.Receive(CTestPacket(XCMD_MenuChoice)
            .Dword(menu.dwSerial)
            .Word(menu.wContext)
            .Word(wLine)
            .Word(0)    // The item ID of the entry.
            .Word(0)    // Its hue.
            .Get());
    }

    int64 MenuTag(const CFakeClient& client)
    {
        return client.GetChar()->m_TagDefs.GetKeyNum("FIXTURE_MENU");
    }
}

TEST_SUITE("Menus")
{
    TEST_CASE("A script menu: each line runs the entry shown there, also after an entry not shown")
    {
        REQUIRE(CTestEngine::LoadScript("t_packetmenu", kScript));
        CTestRunModeScope run;
        CFakeClient client;
        client.Login(CTestEngine::Point(100, 100));

        const SentMenu menu = OpenMenu(client, "m_fixture_menu");
        REQUIRE(menu.bCount == 2);

        // Before, the second line ran the entry not shown.
        SelectLine(client, menu, 2);
        CHECK(MenuTag(client) == 3);

        // The first line and the cancel are as they were.
        SelectLine(client, OpenMenu(client, "m_fixture_menu"), 1);
        CHECK(MenuTag(client) == 1);
        SelectLine(client, OpenMenu(client, "m_fixture_menu"), 0);
        CHECK(MenuTag(client) == -1);
    }

    TEST_CASE("A script menu: a line past the entries sent runs nothing")
    {
        REQUIRE(CTestEngine::LoadScript("t_packetmenu", kScript));
        CTestRunModeScope run;
        CFakeClient client;
        client.Login(CTestEngine::Point(100, 100));

        // Before, the third line ran the third ON=, never shown.
        SelectLine(client, OpenMenu(client, "m_fixture_menu"), 3);
        CHECK(MenuTag(client) == 0);
        SelectLine(client, OpenMenu(client, "m_fixture_menu"), 0xffff);
        CHECK(MenuTag(client) == 0);
    }

    TEST_CASE("A menu: the lines past the entries sent don't find those of an earlier menu")
    {
        REQUIRE(CTestEngine::LoadScript("t_packetmenu", kScript));
        CTestRunModeScope run;
        CFakeClient client;
        client.Login(CTestEngine::Point(100, 100));

        // The longer menu leaves its third entry, the shorter one doesn't send one. Before, its third line found the
        //  entry of the first menu (as a tracking menu finds the UIDs of the targets of the one before).
        REQUIRE(OpenMenu(client, "m_fixture_menu_long").bCount == 3);
        const SentMenu menu = OpenMenu(client, "m_fixture_menu");
        REQUIRE(menu.bCount == 2);
        SelectLine(client, menu, 3);
        CHECK(MenuTag(client) == 0);
    }
}
