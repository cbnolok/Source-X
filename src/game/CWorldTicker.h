/**
* @file CWorldTicker.h
*/

#ifndef _INC_CWORLDTICKER_H
#define _INC_CWORLDTICKER_H

#include "CTimedFunctionHandler.h"
#include "CTimedObject.h"

/*
 * #include <flat_containers/flat_set.hpp>
*/

/*
//--- Include phmap.h
#ifdef ADDRESS_SANITIZER
    #define MYASAN_
#endif

#ifdef _WIN32
//    #define MYSRWLOCK_
    #undef SRWLOCK_INIT
#endif
#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wshift-count-overflow"
#endif

// TODO: undef is TEMPORARY !! There's a bug that needs to be solved
#undef ADDRESS_SANITIZER
#include <parallel_hashmap/phmap.h>

#ifdef MYASAN_
    #define ADDRESS_SANITIZER
#endif
//#ifdef MYSRWLOCK_
//#   define SRWLOCK_INIT
//#endif

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif
//--- End of phmap.h inclusion
*/


class CObjBase;
class CChar;
class CWorldClock;

class CWorldTicker
{
public:
    static const char* m_sClassName;
    CWorldTicker(CWorldClock* pClock);
    ~CWorldTicker() = default;

private:
    using TickingTimedObjEntry = std::pair<int64, CTimedObject*>;
    struct WorldTickList : public std::vector<TickingTimedObjEntry>
    {
        MT_CMUTEX_DEF;
    };

    using TickingCharEntry = std::pair<int64, CChar*>;
    struct CharTickList : public std::vector<TickingCharEntry>
    {
        MT_CMUTEX_DEF;
    };

    //struct StatusUpdatesList : public phmap::parallel_flat_hash_set<CObjBase*>
    //struct StatusUpdatesList : public fc::flat_set<CObjBase*>
    //struct StatusUpdatesList : public std::unordered_set<CObjBase*>
    struct StatusUpdatesList : public std::vector<CObjBase*>
    {
        MT_CMUTEX_DEF;
    };

    WorldTickList _mWorldTickList;
    CharTickList _mCharTickList;

    friend class CWorldTickingList;
    StatusUpdatesList _ObjStatusUpdates;   // objects that need OnTickStatusUpdate called
    std::vector<CObjBase*> _vecObjStatusUpdateEraseRequested;

    // Reuse the same container (using void pointers statically casted) to avoid unnecessary reallocations.
    std::vector<void*> _vecGenericObjsToTick;
    std::vector<size_t> _vecIndexMiscBuffer;

    std::vector<TickingTimedObjEntry> _vecWorldObjsAddRequested;
    std::vector<TickingTimedObjEntry> _vecWorldObjsEraseRequested;
    std::vector<TickingTimedObjEntry> _vecWorldObjsElementBuffer;

    std::vector<TickingCharEntry> _vecPeriodicCharsToAddToList;
    std::vector<TickingCharEntry> _vecPeriodicCharsToEraseFromList;
    std::vector<TickingCharEntry> _vecPeriodicCharsElementBuffer;

    //----

    friend class CWorld;
    friend class CWorldTimedFunctions;
    CTimedFunctionHandler _TimedFunctions; // CTimedFunction Container/Wrapper

    CWorldClock* _pWorldClock;
    int64        _iLastTickDone;

public:
    void Tick();

    void AddTimedObject(int64 iTimeout, CTimedObject* pTimedObject, bool fForce);
    void DelTimedObject(CTimedObject* pTimedObject);
    void AddCharTicking(CChar* pChar, bool fNeedsLock);
    void DelCharTicking(CChar* pChar, bool fNeedsLock);
    void AddObjStatusUpdate(CObjBase* pObj, bool fNeedsLock);
    void DelObjStatusUpdate(CObjBase* pObj, bool fNeedsLock);

private:
    void _InsertTimedObject(const int64 iTimeout, CTimedObject* pTimedObject);
    void _RemoveTimedObject(const int64 iOldTimeout, CTimedObject* pTimedObject);
    void _InsertCharTicking(const int64 iTickNext, CChar* pChar);
    void _RemoveCharTicking(const int64 iOldTimeout, CChar* pChar);
};

#endif // _INC_CWORLDTICKER_H
