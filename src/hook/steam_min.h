// Minimal Steamworks declarations — only what we touch. Not the real SDK headers.
// Layouts verified against Steamworks isteamnetworkingtypes.h / isteamnetworkingmessages.h.
#pragma once
#include <cstdint>

using HSteamUser = int32_t;

// SteamNetworkingIdentity: 136-byte struct.
//   +0  int32 m_eType   (k_ESteamNetworkingIdentityType_SteamID == 1)
//   +4  int32 m_cbSize
//   +8  union { uint64 m_steamID64; ... }   (128-byte union)
// We only read the SteamID64 when m_eType==1.
struct SteamNetworkingIdentity {
    int32_t  m_eType;
    int32_t  m_cbSize;
    union {
        uint64_t m_steamID64;
        uint8_t  m_raw[128];
    };
};
static_assert(sizeof(SteamNetworkingIdentity) == 136, "identity size");
// NOTE: real Steamworks enum — SteamID is 16, not 1 (IPAddress is 1). Confirmed live:
// all sends use idType=16 with the client SteamID at m_steamID64 (offset +8).
constexpr int32_t kIdentityType_SteamID = 16;

// SteamNetworkingMessage_t — the inbound message struct. We only read a few fields by offset
// (verified x64 layout from isteamnetworkingtypes.h). Received via ReceiveMessagesOnChannel.
struct SteamNetworkingMessage_t {
    void*    m_pData;                        // +0   payload
    int32_t  m_cbSize;                       // +8   payload size
    int32_t  m_conn;                         // +12  HSteamNetConnection
    SteamNetworkingIdentity m_identityPeer;  // +16  (136 bytes) sender
    int64_t  m_nConnUserData;                // +152
    int64_t  m_usecTimeReceived;             // +160
    int64_t  m_nMessageNumber;               // +168
    void*    m_pfnFreeData;                  // +176
    void*    m_pfnRelease;                   // +184
    int32_t  m_nChannel;                     // +192
    int32_t  m_nFlags;                       // +196
    int64_t  m_nUserData;                    // +200
    uint16_t m_idxLane;                      // +208
    uint16_t m__pad;
};
static_assert(sizeof(SteamNetworkingMessage_t) >= 210, "message struct");

// int ReceiveMessagesOnChannel(int nLocalChannel, SteamNetworkingMessage_t** ppOut, int nMax)
// returns count; ppOut[0..count-1] are borrowed until Release. We read them post-call.
using ReceiveMessagesOnChannel_t = int32_t (*)(
    void* self, int32_t nLocalChannel, SteamNetworkingMessage_t** ppOut, int32_t nMax);

// ISteamNetworkingMessages vtable (declaration order in isteamnetworkingmessages.h):
//   [0] SendMessageToUser
//   [1] ReceiveMessagesOnChannel
//   [2] AcceptSessionWithUser
//   [3] CloseSessionWithUser
//   [4] CloseChannelWithUser
//   [5] GetSessionConnectionInfo
constexpr int kVT_SendMessageToUser        = 0;
constexpr int kVT_ReceiveMessagesOnChannel = 1;

// EResult return; x64 __fastcall (native member-call ABI). `self` = ISteamNetworkingMessages*.
using SendMessageToUser_t = int32_t (*)(
    void* self,
    const SteamNetworkingIdentity* identityRemote,
    const void* pubData,
    uint32_t cubData,
    int32_t nSendFlags,
    int32_t nRemoteChannel);

// nSendFlags bits we care to log (from isteamnetworkingtypes.h)
constexpr int32_t kSend_Unreliable = 0;
constexpr int32_t kSend_Reliable   = 8;
