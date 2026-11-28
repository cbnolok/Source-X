#ifndef _INC_CTESTCHAT_H
#define _INC_CTESTCHAT_H

#include "CTestPacket.h"
#include "../../../src/common/sphereproto.h"
#include "../../../src/game/game_macros.h"
#include "../../../src/game/CServerConfig.h"
#include <string_view>
#include <vector>

// The chat (FEATURE_T2A_CHAT), with the creation of channels by the players, for the scope.
struct CTestChatScope
{
    const int iFeatureT2A = g_Cfg.m_iFeatureT2A;
    const int iChatFlags = g_Cfg.m_iChatFlags;
    CTestChatScope()
    {
        g_Cfg.m_iFeatureT2A |= FEATURE_T2A_CHAT;
        g_Cfg.m_iChatFlags |= CHATF_CHANNELCREATION;
    }
    ~CTestChatScope()
    {
        g_Cfg.m_iFeatureT2A = iFeatureT2A;
        g_Cfg.m_iChatFlags = iChatFlags;
    }
    CTestChatScope(const CTestChatScope&) = delete;
    CTestChatScope& operator=(const CTestChatScope&) = delete;
};

// 0xB3: a chat command, in UTF-16 (big endian). Its first character is the action (CHATACT_TYPE).
inline std::vector<byte> ChatCommand(std::string_view svText)
{
    CTestPacket packet = CTestPacket::Variable(XCMD_ChatText);
    packet.String("ENU", 4);
    for (const char c : svText)
        packet.Word(word(byte(c)));
    return packet.Get();
}

#endif // _INC_CTESTCHAT_H
