/**
* @file CNetworkInput.h
* @brief Handles network input from clients.
*/

#ifndef _INC_CNETWORKINPUT_H
#define _INC_CNETWORKINPUT_H

#include "CSocket.h"


class CNetworkThread;
class CNetState;
class Packet;
class PacketManager;

class CNetworkInput
{
private:
    CNetworkThread* m_thread;	// owning network thread
    byte* m_receiveBuffer;		// buffer for received data
    byte* m_decryptBuffer;		// buffer for decrypted data

public:
    static const char* m_sClassName;
    CNetworkInput(void);
    ~CNetworkInput(void);

private:
    CNetworkInput(const CNetworkInput& copy);
    CNetworkInput& operator=(const CNetworkInput& other);

public:
    void setOwner(CNetworkThread* thread);   // set owner thread
    bool processInput(void);			    // process input from clients, returns true if work was done

private:
    bool checkForData(fd_set& fds); // check for states which have pending data to read
    void receiveData();             // receive raw data for all sockets
    void processData();             // process received data for all sockets

    bool processRawData(CNetState* state, Packet* buffer);              // process the raw data received, as much as possible; false if refused (read closed)
    bool processData(CNetState* state, Packet* buffer);                 // process received data
    bool processUnknownClientData(CNetState* state, Packet* buffer);    // process data from an unknown client type
    bool processOtherClientData(CNetState* state, Packet* buffer);      // process data from a non-game client
    bool processGameClientData(CNetState* state, Packet* buffer);       // process data from a game client
    // The packets of the decrypted data of a game client, each to its handler (after the packet filters of the scripts),
    //  until the data ends, a packet isn't whole yet or the client is closing. An exception of a handler is counted (too
    //  many disconnect the client). Returns the length left, waiting for more data.
    static uint processGamePackets(CNetState* state, Packet* packet, const PacketManager& packets);

#ifdef UNIT_TESTING
    friend struct CNetTestAccess;   // The fake clients of the unit tests: they run the input on their data.
#endif
};

#endif // _INC_CNETWORKINPUT_H
