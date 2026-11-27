#include "CFakeClient.h"
#include "CTestEngine.h"
#include "../CWorldTestAccess.h"
#include "../../../src/game/chars/CChar.h"
#include "../../../src/game/clients/CAccount.h"
#include "../../../src/game/clients/CClient.h"
#include "../../../src/game/CServer.h"
#include "../../../src/network/CNetworkManager.h"
#include "../../../src/network/CPacketManager.h"
#include <stdexcept>
#include <string>

// What the network manager and the network threads do with a state, for a fake client.
struct CNetTestAccess
{
    // As CNetworkManager::acceptNewConnection with CNetState::init, without a socket.
    static void Open(CNetState* pState, CNetStateTestSink* pSink, dword dwReportedVersion)
    {
        pState->_pUnitTestSink = pSink;
        pState->m_iConnectionTimeMs = CSTime::GetMonotonicSysTimeMilli();
        pState->m_reportedVersionNumber = dwReportedVersion;
        pState->m_peerAddress.SetAddrStr("127.0.0.1");
        g_Serv.StatInc(SERV_STAT_CLIENTS);
        pState->m_client = new CClient(pState);
        pState->m_isReadClosed = false;
        pState->m_isWriteClosed = false;
        pState->m_isInUse = true;
        g_NetworkManager.m_clients.InsertContentHead(pState->m_client);
    }

    // As CNetState::clear, then the garbage collection destroying the client: its char logs out, it leaves the list.
    static void Close(CNetState* pState)
    {
        pState->m_isReadClosed = true;
        pState->m_isWriteClosed = true;
        CClient* pClient = pState->m_client;
        delete pClient;
        pState->m_client = nullptr;
        g_Serv.StatDec(SERV_STAT_CLIENTS);
        pState->m_isInUse = false;
        pState->_pUnitTestSink = nullptr;
    }

    static const PacketManager& GetPackets()
    {
        PacketManager& packets = g_NetworkManager.m_packets;
        if (packets.getHandler(XCMD_GumpDialogRet) == nullptr)
            packets.registerStandardPackets();      // Done by CNetworkManager::start, not run by the tests.
        return packets;
    }
};

namespace
{
    int s_iFakeClients = 0;     // For the IDs of the states and the names of the accounts.
}

CFakeClient::CFakeClient(dword dwReportedVersion) :
    _pState(std::make_unique<CNetState>(0x7000 + s_iFakeClients)), _pAccount(nullptr)
{
    CTestEngine::Init();
    CNetTestAccess::GetPackets();
    CNetTestAccess::Open(_pState.get(), this, dwReportedVersion);
    _pAccount = new CAccount(("fakeclient" + std::to_string(s_iFakeClients)).c_str());   // In the list of the accounts.
    ++s_iFakeClients;
}

CFakeClient::~CFakeClient()
{
    CNetTestAccess::Close(_pState.get());
    // As the server: the account deletes its chars (Delete, with what they carry), then the end of the tick destroys them.
    //  CAccounts::Account_Delete takes the account out of the list, without destroying it.
    g_Accounts.Account_Delete(_pAccount);
    delete _pAccount;
    CWorldTestAccess::DestroyScheduledDeletions();
}

CClient* CFakeClient::GetClient() const noexcept
{
    return _pState->getClient();
}

CChar* CFakeClient::GetChar() const noexcept
{
    CClient* pClient = GetClient();
    return (pClient != nullptr) ? pClient->GetChar() : nullptr;
}

CChar* CFakeClient::Login(const CPointMap& pt, CREID_TYPE id)
{
    CClient* pClient = GetClient();
    pClient->SetConnectType(CONNECT_GAME);
    pClient->m_pAccount = _pAccount;
    _pAccount->OnLogin(pClient);

    CChar* pChar = CChar::CreateBasic(CTestEngine::CharDef(id));
    if (pChar == nullptr)
        throw std::runtime_error("CFakeClient::Login: the char wasn't created");
    pChar->SetName(_pAccount->GetName());
    if (!pChar->SetPlayerAccount(_pAccount) || !pChar->MoveToChar(pt))
        throw std::runtime_error("CFakeClient::Login: the char wasn't placed in the world");
    pClient->addPlayerStart(pChar);
    return pChar;
}

void CFakeClient::Receive(const std::vector<byte>& vData)
{
    // As CNetworkInput::processGameClientData, after the decryption. An unknown packet, or one shorter than its length,
    //  is an error of the test: the server would discard the data, or wait for more.
    const PacketManager& packets = CNetTestAccess::GetPackets();
    Packet packet(vData.data(), uint(vData.size()));
    uint uiRemaining = packet.getRemainingLength();
    while ((uiRemaining > 0) && !_pState->isClosing())
    {
        const byte bId = packet.getRemainingData()[0];
        Packet* pHandler = packets.getHandler(bId);
        if (pHandler == nullptr)
            throw std::invalid_argument("CFakeClient::Receive: no handler for packet " + std::to_string(bId));
        const uint uiLength = pHandler->checkLength(_pState.get(), &packet);
        if (uiLength == 0)
            throw std::invalid_argument("CFakeClient::Receive: wrong length of packet " + std::to_string(bId));
        uiRemaining -= uiLength;

        // The packet filters of the scripts can skip it.
        if (GetClient()->xPacketFilter(packet.getRemainingData(), uiLength))
        {
            packet.skip(int(uiLength));
            continue;
        }

        pHandler->seek();
        pHandler->writeData(packet.getRemainingData(), uiLength);
        packet.skip(int(uiLength));
        pHandler->resize(uiLength);
        pHandler->seek(1);
        pHandler->onReceive(_pState.get());
    }
}

void CFakeClient::OnPacketSent(PacketSend* pPacket)
{
    // As CNetworkOutput: the packet is prepared for the client (onSend, which can drop it), sent, then onSent.
    const std::unique_ptr<PacketSend> pOwned(pPacket);
    CClient* pClient = GetClient();
    if ((pClient == nullptr) || !pOwned->onSend(pClient))
        return;
    const byte* pData = pOwned->getData();
    _vSent.emplace_back(pData, pData + pOwned->getLength());
    pOwned->onSent(pClient);
}

size_t CFakeClient::CountSent(byte bId) const
{
    size_t uiCount = 0;
    for (const std::vector<byte>& vPacket : _vSent)
    {
        if (!vPacket.empty() && (vPacket[0] == bId))
            ++uiCount;
    }
    return uiCount;
}

const std::vector<byte>* CFakeClient::FindLastSent(byte bId) const
{
    for (auto it = _vSent.crbegin(); it != _vSent.crend(); ++it)
    {
        if (!it->empty() && ((*it)[0] == bId))
            return &(*it);
    }
    return nullptr;
}
