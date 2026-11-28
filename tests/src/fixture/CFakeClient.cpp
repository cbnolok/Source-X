#include "CFakeClient.h"
#include "CTestEngine.h"
#include "../CWorldTestAccess.h"
#include "../../../src/common/sphere_library/CSString.h"
#include "../../../src/game/chars/CChar.h"
#include "../../../src/game/clients/CAccount.h"
#include "../../../src/game/clients/CClient.h"
#include "../../../src/game/CServer.h"
#include "../../../src/network/CNetworkInput.h"
#include "../../../src/network/CNetworkManager.h"
#include "../../../src/network/CNetworkOutput.h"
#include "../../../src/network/CPacketManager.h"
#include "../../../src/network/send.h"
#include <stdexcept>
#include <string>

// What the network manager and the network threads do with a state, for a fake client: the server's own functions, the
//  socket aside.
struct CNetTestAccess
{
    // As CNetworkManager::acceptNewConnection with CNetState::init, without a socket: the state opens with a client
    //  (CNetState::initClient), the client joins the list of the clients.
    static void Open(CNetState* pState, CNetStateTestSink* pSink, dword dwReportedVersion)
    {
        pState->_pUnitTestSink = pSink;
        pState->m_peerAddress.SetAddrStr("127.0.0.1");
        pState->m_reportedVersionNumber = dwReportedVersion;
        pState->initClient();
        g_NetworkManager.m_clients.InsertContentHead(pState->getClient());
    }

    // As the network thread closing the connection: CNetState::clear, which schedules the deletion of the client.
    static void Close(CNetState* pState)
    {
        pState->clear();
        pState->_pUnitTestSink = nullptr;
    }

    // As the network input, for the bytes of a tick before the decryption (CNetworkInput::processRawData). False if it
    //  refuses them: the connection is then closed.
    static bool ProcessRaw(CNetState* pState, Packet* pBuffer)
    {
        CNetworkInput input;
        return input.processRawData(pState, pBuffer);
    }

    // As the network input after the decryption (CNetworkInput::processGamePackets): the length left, not processed.
    static uint ProcessGamePackets(CNetState* pState, Packet* pData)
    {
        return CNetworkInput::processGamePackets(pState, pData, GetPackets());
    }
    static int GetPacketExceptions(const CNetState* pState)
    {
        return pState->m_packetExceptions;
    }

    // As the network output: the state takes the packet (CNetState::canReceive, the socket aside), then the checks before
    //  it's sent (CNetworkOutput::checkPacketSend).
    static bool CheckPacketSend(const CNetState* pState, CClient* pClient, PacketSend* pPacket)
    {
        return pState->acceptsPacket(pPacket) && CNetworkOutput::checkPacketSend(pClient, pPacket);
    }

    // CClient::LogIn with the account: its checks, f_onaccount_login, CAccount::OnLogin.
    static byte LogIn(CClient* pClient, CAccount* pAccount, CSString& sMsg)
    {
        return pClient->LogIn(pAccount, sMsg);
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
    // As the server: the connection closes, and the end of the tick destroys the client (its char logs out) with the
    //  objects the test deleted. Then the account deletes its chars (Delete, with what they carry), destroyed in turn.
    //  CAccounts::Account_Delete takes the account out of the list, without destroying it.
    CNetTestAccess::Close(_pState.get());
    CWorldTestAccess::DestroyScheduledDeletions();
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

void CFakeClient::LoginAccount()
{
    // The login message sets the connection type (CCrypto), then the account logs in.
    CClient* pClient = GetClient();
    pClient->SetConnectType(CONNECT_GAME);
    CSString sMsg;
    if (CNetTestAccess::LogIn(pClient, _pAccount, sMsg) != PacketLoginError::Success)
        throw std::runtime_error(std::string("CFakeClient::LoginAccount: the login was refused: ") + sMsg.GetBuffer());
}

CChar* CFakeClient::Login(const CPointMap& pt, CREID_TYPE id)
{
    CClient* pClient = GetClient();
    if (pClient->GetAccount() != _pAccount)
        LoginAccount();

    CChar* pChar = CChar::CreateBasic(CTestEngine::CharDef(id));
    if (pChar == nullptr)
        throw std::runtime_error("CFakeClient::Login: the char wasn't created");
    pChar->SetName(_pAccount->GetName());
    if (!pChar->SetPlayerAccount(_pAccount) || !pChar->MoveToChar(pt))
        throw std::runtime_error("CFakeClient::Login: the char wasn't placed in the world");
    if (pClient->Setup_Start(pChar) != PacketLoginError::Success)
        throw std::runtime_error("CFakeClient::Login: the char's login was refused (@Login)");
    return pChar;
}

void CFakeClient::Receive(const std::vector<byte>& vData)
{
    // An unknown packet first, data ending in a packet shorter than its length, or an exception in a handler, is an error
    //  of the test: the server would discard the data, wait for more, or log the exception and go on (too many disconnect
    //  the client).
    if (vData.empty() || (CNetTestAccess::GetPackets().getHandler(vData[0]) == nullptr))
        throw std::invalid_argument("CFakeClient::Receive: no handler for packet " + (vData.empty() ? std::string("(none)") : std::to_string(vData[0])));

    Packet packet(vData.data(), uint(vData.size()));
    const int iExceptions = CNetTestAccess::GetPacketExceptions(_pState.get());
    const uint uiRemaining = CNetTestAccess::ProcessGamePackets(_pState.get(), &packet);
    if (CNetTestAccess::GetPacketExceptions(_pState.get()) != iExceptions)
        throw std::runtime_error("CFakeClient::Receive: a packet handler threw an exception (logged)");
    if ((uiRemaining > 0) && !_pState->isClosing())
        throw std::invalid_argument("CFakeClient::Receive: the data ends in a packet shorter than its length");
}

int CFakeClient::GetPacketLength(byte bId) const
{
    Packet* pHandler = CNetTestAccess::GetPackets().getHandler(bId);
    if (pHandler == nullptr)
        return -1;
    Packet packet;     // Read by none of the handlers: some lengths depend on the client version only.
    return int(pHandler->getExpectedLength(_pState.get(), &packet));
}

bool CFakeClient::ReceiveRaw(const std::vector<byte>& vData)
{
    Packet buffer(vData.data(), uint(vData.size()));
    return CNetTestAccess::ProcessRaw(_pState.get(), &buffer);
}

void CFakeClient::OnPacketSent(PacketSend* pPacket)
{
    // As the network output: the checks before the packet is sent (onSend can drop it, and the packet filters of the
    //  scripts), then onSent. The output calls onSend once more as it takes the packet from the queue: a check, with no
    //  side effects.
    const std::unique_ptr<PacketSend> pOwned(pPacket);
    CClient* pClient = GetClient();
    if ((pClient == nullptr) || !CNetTestAccess::CheckPacketSend(_pState.get(), pClient, pOwned.get()))
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
