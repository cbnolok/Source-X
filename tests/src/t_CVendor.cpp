// Buying from and selling to vendors, through the packets of a fake client (0x3B, 0x9F).

#include <doctest/doctest.h>
#include "fixture/CFakeClient.h"
#include "fixture/CTestEngine.h"
#include "fixture/CTestObjects.h"
#include "fixture/CTestPacket.h"
#include "../../src/common/sphereproto.h"
#include "../../src/common/resource/CResourceID.h"
#include "../../src/game/chars/CChar.h"
#include "../../src/game/chars/CCharNPC.h"
#include "../../src/game/clients/CClient.h"
#include "../../src/game/items/CItem.h"
#include "../../src/game/items/CItemContainer.h"
#include "../../src/network/receive.h"

namespace
{
    constexpr ITEMID_TYPE kidBottle = ITEMID_TYPE(0x0f0e);
    constexpr ITEMID_TYPE kidBag = ITEMID_TYPE(0x0e76);
    constexpr int kiPrice = 10;     // The VALUE of the bottle; the vendors have no markup.

    constexpr std::string_view kScript = R"(
[ITEMDEF 0f0e]
DEFNAME=i_fixture_bottle
NAME=fixture bottle
TYPE=t_normal
VALUE=10

[ITEMDEF 0e76]
DEFNAME=i_fixture_bag
NAME=fixture bag
TYPE=t_container
)";

    // A vendor NPC at that point. A player vendor is a hired one (STATF_PET): it sells its own items, not copies.
    CChar* CreateVendor(CTestObjects& objs, const CPointMap& pt, bool fPlayerVendor = false)
    {
        REQUIRE(CTestEngine::LoadScript("t_cvendor", kScript));
        CChar* pVendor = objs.Keep(CChar::CreateBasic(CTestEngine::CharDef(CREID_MAN)));
        REQUIRE(pVendor != nullptr);
        REQUIRE(pVendor->SetNPCBrain(NPCBRAIN_VENDOR));
        // No markup. The tag is kept at 0: without it, the markup is read from the region (the engine has none).
        pVendor->m_TagDefs.SetNum("VENDORMARKUP", 0, false);
        if (fPlayerVendor)
            pVendor->StatFlag_Set(STATF_PET);
        REQUIRE(pVendor->MoveToChar(pt));
        return pVendor;
    }

    CItem* CreateItem(ITEMID_TYPE id, CItemContainer* pCont, word wAmount = 1)
    {
        REQUIRE(pCont != nullptr);
        CItem* pItem = CItem::CreateScript(id);
        REQUIRE(pItem != nullptr);
        pItem->SetAmount(wAmount);
        pCont->ContentAdd(pItem);
        return pItem;
    }

    int CountItems(const CItemContainer* pCont, ITEMID_TYPE id)
    {
        return pCont->ContentCount(CResourceID(RES_ITEMDEF, id));
    }

    int CountGold(const CItemContainer* pCont)
    {
        return pCont->ContentCount(CResourceID(RES_TYPEDEF, IT_GOLD));
    }

    // 0x3B: the items bought from the vendor, as the client sends them from its buy list.
    std::vector<byte> BuyRequest(const CChar* pVendor, const CItem* pItem, word wAmount)
    {
        return CTestPacket::Variable(XCMD_VendorBuy)
            .Dword(dword(pVendor->GetUID()))
            .Byte(0x02)     // Items bought.
            .Byte(byte(LAYER_VENDOR_STOCK))
            .Dword(dword(pItem->GetUID()))
            .Word(wAmount)
            .Get();
    }

    // 0x9F: the items sold to the vendor, as the client sends them from its sell list.
    std::vector<byte> SellRequest(const CChar* pVendor, std::initializer_list<const CItem*> items)
    {
        CTestPacket packet = CTestPacket::Variable(XCMD_VendorSell);
        packet.Dword(dword(pVendor->GetUID())).Word(word(items.size()));
        for (const CItem* pItem : items)
            packet.Dword(dword(pItem->GetUID())).Word(pItem->GetAmount());
        return packet.Get();
    }
}

TEST_SUITE("Vendors")
{
    TEST_CASE("Buying from an NPC vendor: a copy of an item of its stock")
    {
        CTestRunModeScope run;
        CFakeClient buyer;
        CTestObjects objs;
        CChar* pBuyer = buyer.Login(CTestEngine::Point(100, 100));
        CChar* pVendor = CreateVendor(objs, CTestEngine::Point(101, 100));
        CItem* pStock = CreateItem(kidBottle, pVendor->GetBank(LAYER_VENDOR_STOCK), 5);
        CItemContainer* pPack = pBuyer->GetPackSafe();
        CreateItem(ITEMID_GOLD_C1, pPack, 100);

        buyer.Receive(BuyRequest(pVendor, pStock, 2));
        CHECK(pStock->GetAmount() == 3);
        CHECK(CountItems(pPack, kidBottle) == 2);
        CHECK(CountGold(pPack) == 100 - 2 * kiPrice);
    }

    TEST_CASE("Buying from an NPC vendor: an item that isn't in its stock is refused")
    {
        CTestRunModeScope run;
        CFakeClient buyer, other;
        CTestObjects objs;
        CChar* pBuyer = buyer.Login(CTestEngine::Point(100, 100));
        CChar* pOther = other.Login(CTestEngine::Point(102, 100));
        CChar* pVendor = CreateVendor(objs, CTestEngine::Point(101, 100));
        CreateItem(kidBottle, pVendor->GetBank(LAYER_VENDOR_STOCK), 5);
        CItemContainer* pPack = pBuyer->GetPackSafe();
        CreateItem(ITEMID_GOLD_C1, pPack, 100);

        CItem* pItem = nullptr;
        SUBCASE("in the pack of another char")
        {
            pItem = CreateItem(kidBottle, pOther->GetPackSafe(), 5);
        }
        SUBCASE("in the bank box of another char")
        {
            pItem = CreateItem(kidBottle, pOther->GetBank(), 5);
        }
        SUBCASE("in the list of the items the vendor buys")
        {
            pItem = CreateItem(kidBottle, pVendor->GetBank(LAYER_VENDOR_BUYS), 5);
        }
        SUBCASE("in a container in the stock")
        {
            CItem* pBag = CreateItem(kidBag, pVendor->GetBank(LAYER_VENDOR_STOCK));
            pItem = CreateItem(kidBottle, dynamic_cast<CItemContainer*>(pBag), 5);
        }
        SUBCASE("on the ground")
        {
            pItem = objs.Keep(CItem::CreateScript(kidBottle));
            REQUIRE(pItem != nullptr);
            pItem->SetAmount(5);
            REQUIRE(pItem->MoveToCheck(CTestEngine::Point(101, 101)));
        }
        REQUIRE(pItem != nullptr);

        buyer.Receive(BuyRequest(pVendor, pItem, 2));
        CHECK(pItem->GetAmount() == 5);
        CHECK(CountItems(pPack, kidBottle) == 0);
        CHECK(CountGold(pPack) == 100);
    }

    TEST_CASE("Buying from a player vendor: its own item, and only from its stock")
    {
        CTestRunModeScope run;
        CFakeClient buyer, other;
        CTestObjects objs;
        CChar* pBuyer = buyer.Login(CTestEngine::Point(100, 100));
        CChar* pOther = other.Login(CTestEngine::Point(102, 100));
        CChar* pVendor = CreateVendor(objs, CTestEngine::Point(101, 100), true);
        CItemContainer* pPack = pBuyer->GetPackSafe();
        CreateItem(ITEMID_GOLD_C1, pPack, 100);

        SUBCASE("an item of its stock is moved to the pack of the buyer")
        {
            CItem* pItem = CreateItem(kidBottle, pVendor->GetBank(LAYER_VENDOR_STOCK));
            buyer.Receive(BuyRequest(pVendor, pItem, 1));
            CHECK(pItem->GetContainer() == pPack);
            CHECK(CountGold(pPack) == 100 - kiPrice);
        }
        SUBCASE("an item of another char stays there")
        {
            CItemContainer* pOtherPack = pOther->GetPackSafe();
            CItem* pItem = CreateItem(kidBottle, pOtherPack);
            buyer.Receive(BuyRequest(pVendor, pItem, 1));
            CHECK(pItem->GetContainer() == pOtherPack);
            CHECK(CountGold(pPack) == 100);
        }
    }

    TEST_CASE("Selling to a vendor: the items of the backpack, not the others of the seller")
    {
        CTestRunModeScope run;
        CFakeClient seller;
        CTestObjects objs;
        CChar* pSeller = seller.Login(CTestEngine::Point(100, 100));
        CChar* pVendor = CreateVendor(objs, CTestEngine::Point(101, 100));
        CreateItem(kidBottle, pVendor->GetBank(LAYER_VENDOR_BUYS));     // It buys the bottles.
        pVendor->GetBank()->m_itEqBankBox.m_Check_Amount = 1000;           // Its gold.
        CItemContainer* pPack = pSeller->GetPackSafe();

        CItem* pInPack = CreateItem(kidBottle, pPack);
        CItem* pBag = CreateItem(kidBag, pPack);
        CItem* pInBag = CreateItem(kidBottle, dynamic_cast<CItemContainer*>(pBag));
        CItem* pInBank = CreateItem(kidBottle, pSeller->GetBank());
        const CUID uidInPack = pInPack->GetUID(), uidInBag = pInBag->GetUID();

        seller.Receive(SellRequest(pVendor, {pInPack, pInBag, pInBank}));
        CHECK(uidInPack.ItemFind(true) == nullptr);
        CHECK(uidInBag.ItemFind(true) == nullptr);
        CHECK(pInBank->GetContainer() == pSeller->GetBank());
        CHECK(CountGold(pPack) == 2 * kiPrice);
    }

    TEST_CASE("Selling to a vendor: an item given to it (dragged) is still sold")
    {
        // CChar::NPC_OnItemGive: dropping an item on a vendor sells it, through Event_VendorSell, without the sell list.
        CTestRunModeScope run;
        CFakeClient seller;
        CTestObjects objs;
        CChar* pSeller = seller.Login(CTestEngine::Point(100, 100));
        CChar* pVendor = CreateVendor(objs, CTestEngine::Point(101, 100));
        CreateItem(kidBottle, pVendor->GetBank(LAYER_VENDOR_BUYS));
        pVendor->GetBank()->m_itEqBankBox.m_Check_Amount = 1000;

        CItem* pItem = CItem::CreateScript(kidBottle);
        REQUIRE(pItem != nullptr);
        pSeller->LayerAdd(pItem, LAYER_DRAGGING);
        REQUIRE(pSeller->LayerFind(LAYER_DRAGGING) == pItem);
        const CUID uidItem = pItem->GetUID();
        VendorItem item;
        item.m_serial = uidItem;
        item.m_vcAmount = 1;
        seller.GetClient()->Event_VendorSell(pVendor, &item, 1);
        CHECK(uidItem.ItemFind(true) == nullptr);
        CHECK(CountGold(pSeller->GetPackSafe()) == kiPrice);
    }
}
