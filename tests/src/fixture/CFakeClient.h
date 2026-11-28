#ifndef _INC_CFAKECLIENT_H
#define _INC_CFAKECLIENT_H

#include "../../../src/common/sphereproto.h"
#include "../../../src/network/CNetState.h"
#include "../../../src/game/uo_files/uofiles_enums_creid.h"
#include <memory>
#include <vector>

class CAccount;
class CChar;
class CClient;
struct CPointMap;

// A game client without a socket, for the integration tests (with CTestEngine). It has its own network state, in the
//  list of the clients (it sees the others and they see it), an account, and once logged in a player char.
// The packets the server sends to it are kept (GetSent): the network state gives them to it instead of a network
//  thread, after the checks of the network output. The packets it sends (Receive) go through the network input, after
//  the decryption. The packet transactions are off: each packet comes alone.
// Its destruction closes the connection, then deletes the account and its chars; the objects scheduled for deletion are
//  destroyed (as at the end of a tick): those the test deleted too.
class CFakeClient final : public CNetStateTestSink
{
public:
    static constexpr dword kdwDefaultVersion = MINCLIVER_TOL;   // 7.0.45.65

    explicit CFakeClient(dword dwReportedVersion = kdwDefaultVersion);
    ~CFakeClient() override;

    CFakeClient(const CFakeClient&) = delete;
    CFakeClient& operator=(const CFakeClient&) = delete;

    CClient* GetClient() const noexcept;
    CNetState* GetNetState() const noexcept
    {
        return _pState.get();
    }
    CAccount* GetAccount() const noexcept
    {
        return _pAccount;
    }
    CChar* GetChar() const noexcept;

    // The account logs in, as after the game login (0x91) and before the char selection (CClient::LogIn): the client is
    //  in game mode, with no char.
    void LoginAccount();
    // A new player char of the account enters the world at that point, as after the char creation: it's placed (logged
    //  out), then CClient::Setup_Start (@Login, the login messages). The account logs in first if it isn't. The objects
    //  need a UID: call it in a CTestRunModeScope.
    CChar* Login(const CPointMap& pt, CREID_TYPE id = CREID_MAN);

    // The client sends these bytes: one or more packets, through the network input (CNetworkInput::processGamePackets).
    void Receive(const std::vector<byte>& vData);
    // The length of that packet, for this client, as the network input reads it: 0 for a packet of variable length (its
    //  length follows the ID), -1 if the server has no handler for it.
    int GetPacketLength(byte bId) const;

    // The client sends these bytes before it's set up, as the network input receives them in a tick: the seed, the
    //  first (encrypted) messages. False if the input refuses them, and the connection is closed.
    bool ReceiveRaw(const std::vector<byte>& vData);

    // The packets the server sent, in order.
    const std::vector<std::vector<byte>>& GetSent() const noexcept
    {
        return _vSent;
    }
    size_t CountSent(byte bId) const;
    const std::vector<byte>* FindLastSent(byte bId) const;
    void ClearSent() noexcept
    {
        _vSent.clear();
    }

private:
    void OnPacketSent(PacketSend* pPacket) override;

    std::unique_ptr<CNetState> _pState;
    CAccount* _pAccount;
    std::vector<std::vector<byte>> _vSent;
};

#endif // _INC_CFAKECLIENT_H
