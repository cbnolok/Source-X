#include "../common/CException.h"
#include "../sphere/threads.h"
#include "../sphere/ProfileTask.h"
#include "chars/CChar.h"
#include "items/CItem.h"
#include "CSector.h"
#include "CWorldClock.h"
#include "CWorldGameTime.h"
#include "CWorldTicker.h"

#ifdef _DEBUG
#   define DEBUG_CTIMEDOBJ_TIMED_TICKING
#   define DEBUG_CCHAR_PERIODIC_TICKING
//#   define DEBUG_STATUSUPDATES
#   define DEBUG_LIST_OPS
//#   define BENCHMARK_LISTS // TODO
#endif

CWorldTicker::CWorldTicker(CWorldClock *pClock)
{
    ASSERT(pClock);
    _pWorldClock = pClock;

    _iLastTickDone = 0;

    _vecGenericObjsToTick.reserve(50);
    _vecWorldObjsEraseRequested.reserve(50);
    _vecPeriodicCharsToEraseFromList.reserve(25);
}


// CTimedObject TIMERs

void CWorldTicker::_InsertTimedObject(const int64 iTimeout, CTimedObject* pTimedObject)
{
    ASSERT(pTimedObject);
    ASSERT(iTimeout != 0);

    const TickingTimedObjEntry entryToAdd(iTimeout, pTimedObject);
    const auto itFoundAddRequest = std::find(
        _vecWorldObjsAddRequested.begin(),
        _vecWorldObjsAddRequested.end(),
        entryToAdd
        );
    if (_vecWorldObjsAddRequested.end() != itFoundAddRequest)
    {
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[%p] WARN: CTimedObj insertion into ticking list already requested.\n", (void*)pTimedObject);
#endif
        return; // Already requested the addition.
    }

    const auto itFoundEraseRequest = std::find(
        _vecWorldObjsEraseRequested.begin(),
        _vecWorldObjsEraseRequested.end(),
        entryToAdd
        );
    if (_vecWorldObjsEraseRequested.end() != itFoundEraseRequest)
    {
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[%p] WARN: Stopped attempt of inserting a CTimedObj which removal from ticking list has been requested!\n", (void*)pTimedObject);
#endif
        return; // Already requested the addition.
    }

    const auto itFound = std::find(
        _mWorldTickList.begin(),
        _mWorldTickList.end(),
        entryToAdd
        );
    if (_mWorldTickList.end() != itFound)
    {
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[%p] WARN: Requested insertion of a CTimedObj already in the ticking list.\n", (void*)pTimedObject);
#endif
        return; // Already requested the addition.
    }

    _vecWorldObjsAddRequested.emplace_back(std::move(entryToAdd));
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
    g_Log.EventDebug("[%p] -STATUS: Done adding CTimedObj in the ticking list.\n", (void*)pTimedObject);
#endif
}

void CWorldTicker::_RemoveTimedObject(const int64 iOldTimeout, CTimedObject* pTimedObject)
{
    ASSERT(iOldTimeout != 0);

    //g_Log.EventDebug("Trying to erase TimedObject 0x%p with old timeout %ld.\n", pTimedObject, iOldTimeout);
#if MT_ENGINES
    std::unique_lock<std::shared_mutex> lock(_mWorldTickList.MT_CMUTEX);
#endif
    const TickingTimedObjEntry entryToRemove(iOldTimeout, pTimedObject);
    const auto itRemoveFound = std::find(
        _vecWorldObjsEraseRequested.begin(),
        _vecWorldObjsEraseRequested.end(),
        entryToRemove
        );
    if (_vecWorldObjsEraseRequested.end() != itRemoveFound)
    {
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[%p] WARN: CTimedObj removal from ticking list already requested.\n", (void*)pTimedObject);
#endif
        return; // Already requested the removal.
    }

    // Check if it's in the ticking list.
    // TODO: use binary search here?
    const auto itAddList = std::find(
        _vecWorldObjsAddRequested.begin(),
        _vecWorldObjsAddRequested.end(),
        entryToRemove
        );
    if (itAddList != _vecWorldObjsAddRequested.end())
    {
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[%p] INFO: Removing CTimedObj from the ticking list add buffer.\n", (void*)pTimedObject);
#endif
        _vecWorldObjsAddRequested.erase(itAddList);
    }

    const auto itTickList = std::find(
        _mWorldTickList.begin(),
        _mWorldTickList.end(),
        entryToRemove
        );
    if (itTickList == _mWorldTickList.end())
    {
        // Not found. The object might have a timeout while being in a non-tickable state, so it isn't in the list.
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[%p] WARN: Requested erasure of TimedObject in mWorldTickList, but it wasn't found.\n", (void*)pTimedObject);
#endif
        return;
    }

    _vecWorldObjsEraseRequested.emplace_back(std::move(entryToRemove));
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
    g_Log.EventDebug("[%p] -STATUS: Done removing CTimedObj in the ticking list.\n", (void*)pTimedObject);
#endif
}

void CWorldTicker::AddTimedObject(const int64 iTimeout, CTimedObject* pTimedObject, bool fForce)
{
    //if (iTimeout < CWorldGameTime::GetCurrentTime().GetTimeRaw())    // We do that to get them tick as sooner as possible
    //    return;

#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
    g_Log.EventDebug("[%p] --STATUS: Trying to add CTimedObj to the ticking list.\n", (void*)pTimedObject);
#endif

    EXC_TRY("AddTimedObject");
    const ProfileTask timersTask(PROFILE_TIMERS);

    EXC_SET_BLOCK("Already ticking?");
    const int64 iTickOld = pTimedObject->_GetTimeoutRaw();
    if (iTickOld != 0)
    {
        // Adding an object already on the list? Am i setting a new timeout without deleting the previous one?
        EXC_SET_BLOCK("Remove");
        _RemoveTimedObject(iTickOld, pTimedObject);
    }

    EXC_SET_BLOCK("Insert");
    bool fCanTick;
    if (fForce)
    {
        fCanTick = true;
    }
    else
    {
        fCanTick = pTimedObject->_CanTick();
        if (!fCanTick)
        {
            if (auto pObjBase = dynamic_cast<const CObjBase*>(pTimedObject))
            {
                // Not yet placed in the world? We could have set the TIMER before setting its P or CONT, we can't know at this point...
                // In this case, add it to the list and check if it can tick in the tick loop. We have maybe useless object in the ticking list and this hampers
                //  performance, but it would be a pain to fix every script by setting the TIMER only after the item is placed in the world...
                fCanTick = !pObjBase->GetTopLevelObj()->GetTopPoint().IsValidPoint();
            }
        }
    }

    if (fCanTick)
    {
        _InsertTimedObject(iTimeout, pTimedObject);
    }

    EXC_CATCH;
}

void CWorldTicker::DelTimedObject(CTimedObject* pTimedObject)
{
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
    g_Log.EventDebug("[%p] --STATUS: Trying to remove CTimedObj from the ticking list.\n", (void*)pTimedObject);
#endif

    EXC_TRY("DelTimedObject");
    const ProfileTask timersTask(PROFILE_TIMERS);

    EXC_SET_BLOCK("Not ticking?");
    const int64 iTickOld = pTimedObject->_GetTimeoutRaw();
    if (iTickOld == 0)
    {
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[%p] WARN: Requested deletion of CTimedObj, but Timeout is 0, so it shouldn't be in the list.\n", (void*)pTimedObject);
        const auto itTickList = std::find_if(
            _mWorldTickList.begin(),
            _mWorldTickList.end(),
            [pTimedObject](const TickingTimedObjEntry& entry) {
                return entry.second == pTimedObject;
            }
            );
        if (itTickList != _mWorldTickList.end()) {
            g_Log.EventDebug("[%p] WARN:   But i have found it in the list! With Timeout %" PRId64 ".\n", (void*)pTimedObject, itTickList->first);
            ASSERT(false);
        }
        else
            g_Log.EventDebug("[%p] WARN:   (rightfully) i haven't found it in the list.\n", (void*)pTimedObject);
#endif
        return;
    }

    EXC_SET_BLOCK("Remove");
    _RemoveTimedObject(iTickOld, pTimedObject);

    EXC_CATCH;
}


// CChar Periodic Ticks (it's a different thing than TIMER!)

void CWorldTicker::_InsertCharTicking(const int64 iTickNext, CChar* pChar)
{
    ASSERT(iTickNext != 0);
    ASSERT(pChar);

#if MT_ENGINES
    std::unique_lock<std::shared_mutex> lock(_mCharTickList.MT_CMUTEX);
#endif

    const TickingCharEntry entryToAdd(iTickNext, pChar);
    const auto itFound = std::find(
        _vecPeriodicCharsToAddToList.begin(),
        _vecPeriodicCharsToAddToList.end(),
        entryToAdd
        );
    if (_vecPeriodicCharsToAddToList.end() != itFound)
    {
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
        g_Log.EventDebug("[%p] WARN: Periodic char insertion into ticking list already requested.\n", (void*)pChar);
#endif
        return; // Already requested the addition.
    }

    const auto itFoundEraseRequest = std::find(
        _vecPeriodicCharsToEraseFromList.begin(),
        _vecPeriodicCharsToEraseFromList.end(),
        entryToAdd
        );
    if (_vecPeriodicCharsToEraseFromList.end() != itFoundEraseRequest)
    {
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
        g_Log.EventDebug("[%p] WARN: Stopped insertion attempt of a CChar which removal from periodic ticking list has been requested!\n", (void*)pChar);
#endif
        return; // Already requested the addition.
    }

#ifdef _DEBUG
    const auto itTickAddListOnlyChar = std::find_if(
        _vecPeriodicCharsToAddToList.begin(),
        _vecPeriodicCharsToAddToList.end(),
        [pChar](const TickingCharEntry& entry) {
            return entry.second == pChar;
        }
        );
    ASSERT(_vecPeriodicCharsToAddToList.end() == itTickAddListOnlyChar);

    const auto itTickListOnlyChar = std::find_if(
        _mCharTickList.begin(),
        _mCharTickList.end(),
        [pChar](const TickingCharEntry& entry) {
            return entry.second == pChar;
        }
        );
    ASSERT(_mCharTickList.end() == itTickListOnlyChar);
#endif

    _vecPeriodicCharsToAddToList.emplace_back(std::move(entryToAdd));
    pChar->_iTimePeriodicTick = iTickNext;
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
    g_Log.EventDebug("[%p] -STATUS: Done adding the CChar to the periodic ticking list add buffer.\n", (void*)pChar);
#endif
}

bool CWorldTicker::_RemoveCharTicking(const int64 iOldTimeout, CChar* pChar)
{
    ASSERT(iOldTimeout != 0);
    ASSERT(pChar);

    // I'm reasonably sure that the element i'm trying to remove is present in this container.
#if MT_ENGINES
    std::unique_lock<std::shared_mutex> lock(_mCharTickList.MT_CMUTEX);
#endif

    const TickingCharEntry entryToRemove(iOldTimeout, pChar);
    const auto itRemoveFound = std::find(
        _vecPeriodicCharsToEraseFromList.begin(),
        _vecPeriodicCharsToEraseFromList.end(),
        entryToRemove
        );
    if (_vecPeriodicCharsToEraseFromList.end() != itRemoveFound)
    {
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
        g_Log.EventDebug("[%p] WARN: Periodic char erasure from ticking list already requested.\n", (void*)pChar);
#endif
        return false; // Already requested the removal.
    }

    bool fRemovedFromAddList = false;
    const auto itAddFound = std::find(
        _vecPeriodicCharsToAddToList.begin(),
        _vecPeriodicCharsToAddToList.end(),
        entryToRemove
        );
    if (_vecPeriodicCharsToAddToList.end() != itAddFound)
    {
        fRemovedFromAddList = true;
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
        g_Log.EventDebug("[%p] INFO: Erasing char from periodic ticking list add buffer.\n", (void*)pChar);
#endif
        _vecPeriodicCharsToAddToList.erase(itAddFound);
    }

    // Check if it's in the ticking list.
    const auto itTickList = std::find(
        _mCharTickList.begin(),
        _mCharTickList.end(),
        entryToRemove
        );
    if (itTickList == _mCharTickList.end() && !fRemovedFromAddList)
    {
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
        g_Log.EventDebug("[%p] WARN: Requested Periodic char erasure from ticking list, but not found.\n", (void*)pChar);
#endif
        return false;
    }

#ifdef _DEBUG
    bool fRemovedFromTickList = (_vecPeriodicCharsToAddToList.end() == itAddFound);
    ASSERT(fRemovedFromTickList || fRemovedFromAddList);
#endif

    _vecPeriodicCharsToEraseFromList.emplace_back(std::move(entryToRemove));
    pChar->_iTimePeriodicTick = 0;
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
    g_Log.EventDebug("[%p] -STATUS: Done adding the CChar to the periodic ticking list remove buffer.\n", (void*)pChar);
#endif
    return true;
}

void CWorldTicker::AddCharTicking(CChar* pChar, bool fNeedsLock)
{
    EXC_TRY("AddCharTicking");

    const ProfileTask timersTask(PROFILE_TIMERS);

    int64 iTickNext, iTickOld;
    if (fNeedsLock)
    {
#if MT_ENGINES
        std::unique_lock<std::shared_mutex> lock(pChar->MT_CMUTEX);
#endif
        iTickNext = pChar->_iTimeNextRegen;
        iTickOld = pChar->_iTimePeriodicTick;
    }
    else
    {
        iTickNext = pChar->_iTimeNextRegen;
        iTickOld = pChar->_iTimePeriodicTick;
    }

#ifdef DEBUG_CCHAR_PERIODIC_TICKING
    g_Log.EventDebug("[%p] --STATUS: Trying to add CChar to the periodic ticking list. Tickold: %" PRId64 ". Ticknext: %" PRId64 ".\n",
        (void*)pChar, iTickOld, iTickNext);
#endif

    if (iTickNext == iTickOld)
    {
/*
#ifdef _DEBUG
        auto it = std::find_if(_mCharTickList.begin(), _mCharTickList.end(),
            [pChar](const std::pair<int64, CChar*>& elem) {
                return elem.second == pChar;
            });
        DEBUG_ASSERT(it == _mCharTickList.end());
#endif
*/
        g_Log.EventDebug("[%p] WARN: Stop, tickold == ticknext.\n", (void*)pChar);
        return;
    }

    //if (iTickNext < CWorldGameTime::GetCurrentTime().GetTimeRaw())    // We do that to get them tick as sooner as possible
    //    return;

    if (iTickOld != 0)
    {
        // Adding an object already on the list? Am i setting a new timeout without deleting the previous one?
        EXC_SET_BLOCK("Remove");
        const bool fRet = _RemoveCharTicking(iTickOld, pChar);
        UnreferencedParameter(fRet);
        DEBUG_ASSERT(fRet);
    }

    EXC_SET_BLOCK("Insert");
    _InsertCharTicking(iTickNext, pChar);

    EXC_CATCH;
}

void CWorldTicker::DelCharTicking(CChar* pChar, bool fNeedsLock)
{
    EXC_TRY("DelCharTicking");
    const ProfileTask timersTask(PROFILE_TIMERS);

    int64 iTickOld;
    if (fNeedsLock)
    {
#if MT_ENGINES
        std::unique_lock<std::shared_mutex> lock(pChar->MT_CMUTEX);
#endif
        iTickOld = pChar->_iTimePeriodicTick;
    }
    else
    {
        iTickOld = pChar->_iTimePeriodicTick;
    }

#ifdef DEBUG_CCHAR_PERIODIC_TICKING
    g_Log.EventDebug("[%p] --STATUS: Trying to remove CChar from the periodic ticking list. Tickold: %" PRId64 ".\n",
        (void*)pChar, iTickOld);
#endif

    if (iTickOld == 0)
    {
#ifdef DEBUG_CCHAR_PERIODIC_TICKING
        g_Log.EventDebug("[%p] WARN: Requested deletion of Periodic char, but Timeout is 0, so it shouldn't be in the list.\n", (void*)pChar);
        auto find_fn =
            [pChar](const TickingCharEntry& entry) {
                return entry.second == pChar;
        };

        const auto itTickRemoveList = std::find_if(
            _vecPeriodicCharsToEraseFromList.begin(),
            _vecPeriodicCharsToEraseFromList.end(),
            find_fn
            );
        if (itTickRemoveList != _vecPeriodicCharsToEraseFromList.end())
        {
            g_Log.EventDebug("[%p] WARN:   though, found it in the removal list, so it's fine..\n", (void*)pChar);
            return;
        }

        const auto itTickList = std::find_if(
            _mCharTickList.begin(),
            _mCharTickList.end(),
            find_fn
            );
        if (itTickList != _mCharTickList.end())
        {
            g_Log.EventDebug("[%p] WARN:   But i have found it in the list! With Timeout %" PRId64 ".\n", (void*)pChar, itTickList->first);
            ASSERT(false);
            return;
        }

        g_Log.EventDebug("[%p] WARN:   (rightfully) i haven't found it in any list.\n", (void*)pChar);
#endif
        return;
    }

    EXC_SET_BLOCK("Remove");
    _RemoveCharTicking(iTickOld, pChar);

    EXC_CATCH;
}

void CWorldTicker::AddObjStatusUpdate(CObjBase* pObj, bool fNeedsLock) // static
{
#ifdef DEBUG_STATUSUPDATES
    g_Log.EventDebug("[%p] --STATUS: Trying to add CObjBase to the status update list.\n", (void*)pObj);
#endif
    EXC_TRY("AddObjStatusUpdate");
    const ProfileTask timersTask(PROFILE_TIMERS);

    UnreferencedParameter(fNeedsLock);
    {
#if MT_ENGINES
        std::unique_lock<std::shared_mutex> lock(_ObjStatusUpdates.MT_CMUTEX);
#endif
        //_ObjStatusUpdates.insert(pObj);

        // Here i don't need to use an "add" buffer, like with CTimedObj, because this container isn't meant
        //  to be ordered and i can just push back stuff.
        const auto itDuplicate = std::find(
            _ObjStatusUpdates.begin(),
            _ObjStatusUpdates.end(),
            pObj
            );
        if (_ObjStatusUpdates.end() != itDuplicate)
        {
#ifdef DEBUG_STATUSUPDATES
            g_Log.EventDebug("[%p] WARN: Trying to add status update for duplicate CObjBase.\n", (void*)pObj);
#endif
            return;
        }

        _ObjStatusUpdates.emplace_back(pObj);
#ifdef DEBUG_STATUSUPDATES
        g_Log.EventDebug("[%p] -STATUS: Done adding CObjBase to the status update list.\n", (void*)pObj);
#endif
    }

    EXC_CATCH;
}

void CWorldTicker::DelObjStatusUpdate(CObjBase* pObj, bool fNeedsLock) // static
{
#ifdef DEBUG_STATUSUPDATES
    g_Log.EventDebug("[%p] --STATUS: Trying to remove CObjBase from the status update list.\n", (void*)pObj);
#endif
    EXC_TRY("DelObjStatusUpdate");
    const ProfileTask timersTask(PROFILE_TIMERS);

    UnreferencedParameter(fNeedsLock);
    {
#if MT_ENGINES
        std::unique_lock<std::shared_mutex> lock(_ObjStatusUpdates.MT_CMUTEX);
#endif
        //_ObjStatusUpdates.erase(pObj);

        const auto itMissing = std::find(
            _ObjStatusUpdates.begin(),
            _ObjStatusUpdates.end(),
            pObj
            );
        if (_ObjStatusUpdates.end() == itMissing)
        {
#ifdef DEBUG_STATUSUPDATES
            g_Log.EventDebug("[%p] WARN: Requested erasure of CObjBase from the status update list, but it wasn't found.\n", (void*)pObj);
#endif
            return;
        }

        const auto itRepeated = std::find(
            _vecObjStatusUpdateEraseRequested.begin(),
            _vecObjStatusUpdateEraseRequested.end(),
            pObj
            );
        if (_vecObjStatusUpdateEraseRequested.end() != itRepeated)
        {
#ifdef DEBUG_STATUSUPDATES
            g_Log.EventDebug("WARN [%p]: CObjBase erasure from the status update list already requested.\n", (void*)pObj);
#endif
            return;
        }

        _vecObjStatusUpdateEraseRequested.emplace_back(pObj);
#ifdef DEBUG_STATUSUPDATES
        g_Log.EventDebug("[%p] -STATUS: Done adding CObjBase to the status update remove buffer.\n", (void*)pObj);
#endif
    }

    EXC_CATCH;
}

template <typename T>
static void sortedVecRemoveElementsByIndices(std::vector<T>& vecMain, const std::vector<size_t>& vecIndicesToRemove)
{
    // Erase in chunks, call erase the least times possible.
    if (vecIndicesToRemove.empty())
        return;

    DEBUG_ASSERT(std::is_sorted(vecMain.begin(), vecMain.end()));
    DEBUG_ASSERT(std::is_sorted(vecIndicesToRemove.begin(), vecIndicesToRemove.end()));
    // Check that those sorted vectors do not have duplicated values.
    DEBUG_ASSERT(std::adjacent_find(vecMain.begin(), vecMain.end()) == vecMain.end());
    DEBUG_ASSERT(std::adjacent_find(vecIndicesToRemove.begin(), vecIndicesToRemove.end()) == vecIndicesToRemove.end());

#ifdef DEBUG_LIST_OPS
    g_Log.EventDebug("Starting sortedVecRemoveElementsByIndices.\n");
    // Copy the original vector to check against later
    std::vector<T> originalVecMain = vecMain;
#endif

    // Reverse iterators for vecIndicesToRemove allow us to remove elements from the back of the vector
    // towards the front, which prevents invalidating remaining indices when elements are removed.
    auto itReverseRemoveFirst = vecIndicesToRemove.rbegin(); // Points to the last element in vecIndicesToRemove
    while (itReverseRemoveFirst != vecIndicesToRemove.rend())
    {
        // Find contiguous block
        auto itReverseRemoveLast = itReverseRemoveFirst; // marks the end of a contiguous block of indices to remove.

        // This inner loop identifies a contiguous block of indices to remove.
        // A block is contiguous if the current index *first is exactly 1 greater than the next index.
        auto itReverseRemoveFirst_Next = itReverseRemoveFirst;
        while (
            (++itReverseRemoveFirst_Next != vecIndicesToRemove.rend()) &&   // Ensure std::next(first) doesn't go out of bounds
            (*itReverseRemoveFirst == *itReverseRemoveFirst_Next + 1)       // Check if the next index is 1 less than the current (contiguous)
        )
        {
            itReverseRemoveFirst = itReverseRemoveFirst_Next;
        }

        // Once we find a contiguous block, we erase that block from vecMain.
        // We calculate the range to erase by converting the reverse iterators to normal forward iterators.
        auto itForwardRemoveBegin = vecMain.begin() + *itReverseRemoveFirst;
        auto itForwardRemoveEnd = vecMain.begin() + *itReverseRemoveLast + 1;
        vecMain.erase(itForwardRemoveBegin, itForwardRemoveEnd);

#ifdef DEBUG_LIST_OPS
        const size_t iRemoveFirst = std::distance(vecMain.begin(), itForwardRemoveBegin);
        const size_t iRemoveLast  = std::distance(vecMain.begin(), itForwardRemoveEnd - 1);
        g_Log.EventDebug("Removing contiguous indices: %" PRIuSIZE_T " to %" PRIuSIZE_T " (total sizes vecMain: %" PRIuSIZE_T ", vecIndices: %" PRIuSIZE_T ").\n",
            iRemoveFirst, iRemoveLast, vecMain.size(), vecIndicesToRemove.size());
#endif

        // Move to the next index to check. The above erase operation doesn't invalidate reverse iterators.
        if (itReverseRemoveFirst != vecIndicesToRemove.rend())
        {
            ++itReverseRemoveFirst;
        }
    }

#ifdef DEBUG_LIST_OPS
    // Sanity Check: Verify that the removed elements are no longer present in vecMain
    for (auto index : vecIndicesToRemove) {
        ASSERT(index < originalVecMain.size());
        ASSERT(std::find(vecMain.begin(), vecMain.end(), originalVecMain[index]) == vecMain.end());
    }
#endif

    /*
    // Alternative implementation:
    // We cannot use a vector with the indices but we need a vector with a copy of the elements to remove.
    // std::remove_if in itself is more efficient than multiple erase calls, because the number of the element shifts is lesser.
    // Though, we need to consider the memory overhead of reading through an std::pair of two types, which is bigger than just an index.
    // Also, jumping across the vector in a non-contiguous way with the binary search can add additional memory overhead by itself, and
    //   this will be greater the bigger are the elements in the vector..
    // The bottom line is that we need to run some benchmarks between the two algorithms, and possibly also for two versions of this algorithm,
    //   one using binary search and another with linear search.
    // The latter might actually be faster for a small number of elements, since it's more predictable for the prefetcher.

    // Use std::remove_if to shift elements not marked for removal to the front
    auto it = std::remove_if(vecMain.begin(), vecMain.end(),
        [&](const T& element) {
            // Check if the current element is in the valuesToRemove vector using binary search
            return std::binary_search(valuesToRemove.begin(), valuesToRemove.end(), element);
        });

    // Erase the removed elements in one go
    vecMain.erase(it, vecMain.end());
    */
}

template <typename T>
static bool unsortedVecHasUniqueValues(const std::vector<T>& vec) noexcept {
    for (size_t i = 0; i < vec.size(); ++i) {
        for (size_t j = i + 1; j < vec.size(); ++j) {
            if ((i != j) && (vec[i] == vec[j])) {
                return false; // Found a duplicate
            }
        }
    }
    return true; // No duplicates found
}

template <typename T>
static void unsortedVecRemoveElementsByValues(std::vector<T>& vecMain, const std::vector<T> & vecValuesToRemove)
{
    if (vecValuesToRemove.empty())
        return;

    DEBUG_ASSERT(unsortedVecHasUniqueValues(vecMain));
    DEBUG_ASSERT(unsortedVecHasUniqueValues(vecValuesToRemove));

    // Sort valuesToRemove for binary search
    //std::sort(vecValuesToRemove.begin(), vecValuesToRemove.end());

    // Use std::remove_if to shift elements not marked for removal to the front
    auto it = std::remove_if(vecMain.begin(), vecMain.end(),
        [&](const T& element) {
            // Use binary search to check if the element should be removed
            //return std::binary_search(vecValuesToRemove.begin(), vecValuesToRemove.end(), element);
            return std::find(vecValuesToRemove.begin(), vecValuesToRemove.end(), element) != vecValuesToRemove.end();
        });

    // Erase the removed elements in one go
    vecMain.erase(it, vecMain.end());
}


/*
// To be tested and benchmarked.
template <typename T>
static void sortedVecRemoveElementsByValues(std::vector<T>& vecMain, const std::vector<T>& toRemove)
{
    if (toRemove.empty() || vecMain.empty())
        return;

    auto mainIt = vecMain.begin();
    auto removeIt = toRemove.begin();

    // Destination pointer for in-place shifting
    auto destIt = mainIt;

    while (mainIt != vecMain.end() && removeIt != toRemove.end()) {
        // Skip over elements in the main vector that are smaller than the current element to remove
        auto nextRangeEnd = std::lower_bound(mainIt, vecMain.end(), *removeIt);

        // Batch copy the range of elements not marked for removal
        std::move(mainIt, nextRangeEnd, destIt);
        destIt += std::distance(mainIt, nextRangeEnd);

        // Advance main iterator and remove iterator
        mainIt = nextRangeEnd;

        // Skip the elements that need to be removed
        if (mainIt != vecMain.end() && *mainIt == *removeIt) {
            ++mainIt;
            ++removeIt;
        }
    }

    // Copy the remaining elements if there are any left
    std::move(mainIt, vecMain.end(), destIt);

    // Resize the vector to remove the now extraneous elements at the end
    vecMain.resize(destIt - vecMain.begin());
}
*/

template <typename T>
static void sortedVecRemoveAddQueued(
    std::vector<T> &vecMain, std::vector<T> &vecToRemove, std::vector<T> &vecToAdd, std::vector<T> &vecElemBuffer
    )
{
    DEBUG_ASSERT(std::is_sorted(vecMain.begin(), vecMain.end()));
    DEBUG_ASSERT(std::adjacent_find(vecMain.begin(), vecMain.end()) == vecMain.end()); // no duplicate values

    //EXC_TRY("vecRemoveAddQueued");
    //EXC_SET_BLOCK("Sort intermediate lists");
    std::sort(vecToAdd.begin(), vecToAdd.end());
    std::sort(vecToRemove.begin(), vecToRemove.end());

    DEBUG_ASSERT(std::adjacent_find(vecToAdd.begin(), vecToAdd.end()) == vecToAdd.end()); // no duplicate values
    DEBUG_ASSERT(std::adjacent_find(vecToRemove.begin(), vecToRemove.end()) == vecToRemove.end()); // no duplicate values

    //EXC_SET_BLOCK("Ordered remove");
    if (!vecToRemove.empty())
    {
        // TODO: test and benchmark if the approach of the above function (sortedVecRemoveElementsInPlace) might be faster.
        vecElemBuffer.clear();
        vecElemBuffer.reserve(vecMain.size() / 2);
        std::set_difference(
            vecMain.begin(), vecMain.end(),
            vecToRemove.begin(), vecToRemove.end(),
            std::back_inserter(vecElemBuffer)
            );
        //vecMain.swap(vecElemBuffer);
        vecMain = std::move(vecElemBuffer);
        vecElemBuffer.clear();
        vecToRemove.clear();
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[GLOBAL] STATUS: Nonempty tick list remove buffer processed.\n");
#endif
    }

    //EXC_SET_BLOCK("Mergesort");
    if (!vecToAdd.empty())
    {
        vecElemBuffer.clear();
        vecMain.reserve(vecMain.size() + vecToAdd.size());
        std::merge(
            vecMain.begin(), vecMain.end(),
            vecToAdd.begin(), vecToAdd.end(),
            std::back_inserter(vecElemBuffer)
            );
        //vecMain.swap(vecElemBuffer);
        vecMain = std::move(vecElemBuffer);
        vecElemBuffer.clear();
        vecToAdd.clear();
#ifdef DEBUG_CTIMEDOBJ_TIMED_TICKING
        g_Log.EventDebug("[GLOBAL] STATUS: Nonempty tick list add buffer processed.\n");
#endif
    }

    //EXC_CATCH:
}


// Check timeouts and do ticks
void CWorldTicker::Tick()
{
    ADDTOCALLSTACK("CWorldTicker::Tick");
    EXC_TRY("CWorldTicker::Tick");

    EXC_SET_BLOCK("Once per tick stuff");
    // Do this once per tick.
    //  Update status flags from objects, update current tick.
    if (_iLastTickDone <= _pWorldClock->GetCurrentTick())
    {
        ++_iLastTickDone;   // Update current tick.

        /* process objects that need status updates
        * these objects will normally be in containers which don't have any period _OnTick method
        * called (whereas other items can receive the OnTickStatusUpdate() call via their normal
        * tick method).
        * note: ideally, a better solution to accomplish this should be found if possible
        * TODO: implement a new class inheriting from CTimedObject to get rid of this code?
        */
        {
            {
#if MT_ENGINES
                std::unique_lock<std::shared_mutex> lock_su(_ObjStatusUpdates.MT_CMUTEX);
#endif
                EXC_TRYSUB("StatusUpdates");
                EXC_SETSUB_BLOCK("Remove requested");
                unsortedVecRemoveElementsByValues(_ObjStatusUpdates, _vecObjStatusUpdateEraseRequested);

                EXC_SETSUB_BLOCK("Selection");
                if (!_ObjStatusUpdates.empty())
                {
                    for (CObjBase* pObj : _ObjStatusUpdates)
                    {
                        if (pObj && !pObj->_IsBeingDeleted())
                            _vecGenericObjsToTick.emplace_back(static_cast<void*>(pObj));
                    }
                }
                EXC_CATCHSUB("");
                //EXC_DEBUGSUB_START;
                //EXC_DEBUGSUB_END;
                _ObjStatusUpdates.clear();
                _vecObjStatusUpdateEraseRequested.clear();
            }

            EXC_TRYSUB("StatusUpdates");
            EXC_SETSUB_BLOCK("Loop");
            for (void* pObjVoid : _vecGenericObjsToTick)
            {
                CObjBase* pObj = static_cast<CObjBase*>(pObjVoid);
                pObj->OnTickStatusUpdate();
            }
            EXC_CATCHSUB("");

            _vecGenericObjsToTick.clear();
        }
    }


    /* World ticking (timers) */

    // Items, Chars ... Everything relying on CTimedObject (excepting CObjBase, which inheritance is only virtual)
    int64 iCurTime = CWorldGameTime::GetCurrentTime().GetTimeRaw();    // Current timestamp, a few msecs will advance in the current tick ... avoid them until the following tick(s).

    {
        // Need this new scope to give the right lifetime to ProfileTask.
        EXC_SET_BLOCK("TimedObjects");
        const ProfileTask timersTask(PROFILE_TIMERS);
        {
            // Need here another scope to give the right lifetime to the unique_lock.
#if MT_ENGINES
            std::unique_lock<std::shared_mutex> lock(_mWorldTickList.MT_CMUTEX);
#endif
            {
                // New requests done during the world loop.
                EXC_TRYSUB("Update main list");
                sortedVecRemoveAddQueued(_mWorldTickList, _vecWorldObjsEraseRequested, _vecWorldObjsAddRequested, _vecWorldObjsElementBuffer);
                EXC_CATCHSUB("");
            }

            // Need here a new, inner scope to get rid of EXC_TRYSUB variables
            {
                EXC_TRYSUB("Selection");
                _vecIndexMiscBuffer.clear();
                WorldTickList::iterator itMap = _mWorldTickList.begin();
                WorldTickList::iterator itMapEnd = _mWorldTickList.end();

                size_t uiProgressive = 0;
                int64 iTime;
                while ((itMap != itMapEnd) && (iCurTime > (iTime = itMap->first)))
                {
                    CTimedObject* pTimedObj = itMap->second;
                    if (pTimedObj->_IsTimerSet() && pTimedObj->_CanTick())
                    {
                        if (pTimedObj->_GetTimeoutRaw() <= iCurTime)
                        {
                            if (auto pObjBase = dynamic_cast<const CObjBase*>(pTimedObj))
                            {
                                if (pObjBase->_IsBeingDeleted())
                                    continue;
                            }

                            _vecGenericObjsToTick.emplace_back(static_cast<void*>(pTimedObj));
                            _vecIndexMiscBuffer.emplace_back(uiProgressive);

                            pTimedObj->_ClearTimeout();
                        }
                        //else
                        //{
                        //    // This shouldn't happen... If it does, get rid of the entry on the list anyways,
                        //    //  it got desynchronized in some way and might be an invalid or even deleted and deallocated object!
                        //}
                    }
                    ++itMap;
                    ++uiProgressive;
                }
                EXC_CATCHSUB("");
            }

            {
                EXC_TRYSUB("Delete from List");
                sortedVecRemoveElementsByIndices(_mWorldTickList, _vecIndexMiscBuffer);
                EXC_CATCHSUB("");
            }

            _vecIndexMiscBuffer.clear();
            // Done working with _mWorldTickList, we don't need the lock from now on.
        }

        lpctstr ptcSubDesc;
        for (void* pObjVoid : _vecGenericObjsToTick)    // Loop through all msecs stored, unless we passed the timestamp.
        {
            ptcSubDesc = "Generic";

            EXC_TRYSUB("Tick");
            EXC_SETSUB_BLOCK("Elapsed");

            CTimedObject* pTimedObj = static_cast<CTimedObject*>(pObjVoid);

#if MT_ENGINES
            std::unique_lock<std::shared_mutex> lockTimeObj(pTimedObj->MT_CMUTEX);
#endif

            const PROFILE_TYPE profile = pTimedObj->_GetProfileType();
            const ProfileTask  profileTask(profile);

            // Default to true, so if any error occurs it gets deleted for safety
            //  (valid only for classes having the Delete method, which, for everyone to know, does NOT destroy the object).
            bool fDelete = true;

            switch (profile)
            {
                case PROFILE_ITEMS:
                {
                    CItem* pItem = dynamic_cast<CItem*>(pTimedObj);
                    ASSERT(pItem);
                    if (pItem->IsItemEquipped())
                    {
                        ptcSubDesc = "ItemEquipped";
                        CObjBaseTemplate* pObjTop = pItem->GetTopLevelObj();
                        ASSERT(pObjTop);

                        CChar* pChar = dynamic_cast<CChar*>(pObjTop);
                        if (pChar)
                        {
                            fDelete = !pChar->OnTickEquip(pItem);
                            break;
                        }

                        ptcSubDesc = "Item (fallback)";
                        g_Log.Event(LOGL_CRIT, "Item equipped, but not contained in a character? (UID: 0%" PRIx32 ")\n.", pItem->GetUID().GetObjUID());
                    }
                    else
                    {
                        ptcSubDesc = "Item";
                    }
                    fDelete = (pItem->_OnTick() == false);
                    break;
                }
                break;

                case PROFILE_CHARS:
                {
                    ptcSubDesc = "Char";
                    CChar* pChar = dynamic_cast<CChar*>(pTimedObj);
                    ASSERT(pChar);
                    fDelete = !pChar->_OnTick();
                    if (!fDelete && pChar->m_pNPC && !pTimedObj->_IsTimerSet())
                    {
                        pTimedObj->_SetTimeoutS(3);   //3 seconds timeout to keep NPCs 'alive'
                    }
                }
                break;

                case PROFILE_SECTORS:
                {
                    ptcSubDesc = "Sector";
                    fDelete = false;    // sectors should NEVER be deleted.
                    pTimedObj->_OnTick();
                }
                break;

                case PROFILE_MULTIS:
                {
                    ptcSubDesc = "Multi";
                    fDelete = !pTimedObj->_OnTick();
                }
                break;

                case PROFILE_SHIPS:
                {
                    ptcSubDesc = "ItemShip";
                    fDelete = !pTimedObj->_OnTick();
                }
                break;

                case PROFILE_TIMEDFUNCTIONS:
                {
                    ptcSubDesc = "TimedFunction";
                    fDelete = false;
                    pTimedObj->_OnTick();
                }
                break;

                default:
                {
                    ptcSubDesc = "Default";
                    fDelete = !pTimedObj->_OnTick();
                }
                break;
            }

            if (fDelete)
            {
                EXC_SETSUB_BLOCK("Delete");
                CObjBase* pObjBase = dynamic_cast<CObjBase*>(pTimedObj);
                ASSERT(pObjBase); // Only CObjBase-derived objects have the Delete method, and should be Delete-d.
                pObjBase->Delete();
            }

            EXC_CATCHSUB(ptcSubDesc);
        }

    }

    _vecGenericObjsToTick.clear();

    // ----

    /* Periodic, automatic ticking for every char */

    // No need another scope here to encapsulate this ProfileTask, because from now on, to the end of this method,
    //  everything we do is related to char-only stuff.
    EXC_SET_BLOCK("Char Periodic Ticks");
    const ProfileTask taskChars(PROFILE_CHARS);
    {
        // Need here another scope to give the right lifetime to the unique_lock.
#if MT_ENGINES
        std::unique_lock<std::shared_mutex> lock(_mCharTickList.MT_CMUTEX);
#endif
        {
            // New requests done during the world loop.
            EXC_TRYSUB("Update main list");
            sortedVecRemoveAddQueued(_mCharTickList, _vecPeriodicCharsToEraseFromList, _vecPeriodicCharsToAddToList, _vecPeriodicCharsElementBuffer);
            EXC_CATCHSUB("");
        }

        {
            EXC_TRYSUB("Selection");
#ifdef _DEBUG
            g_Log.EventDebug("Start looping through char periodic ticks.\n");
#endif
            CharTickList::iterator itMap       = _mCharTickList.begin();
            CharTickList::iterator itMapEnd    = _mCharTickList.end();

            size_t uiProgressive = 0;
            int64 iTime;
            while ((itMap != itMapEnd) && (iCurTime > (iTime = itMap->first)))
            {
                CChar* pChar = itMap->second;
#ifdef _DEBUG
                g_Log.EventDebug("Executing char periodic tick: %p. Registered time: %" PRId64 ". pChar->_iTimePeriodicTick: %" PRId64 "\n",
                    (void*)pChar, itMap->first, pChar->_iTimePeriodicTick);
#endif
                if ((pChar->_iTimePeriodicTick != 0) && pChar->_CanTick() && !pChar->_IsBeingDeleted())
                {
                    if (pChar->_iTimePeriodicTick <= iCurTime)
                    {
                        _vecGenericObjsToTick.emplace_back(static_cast<void*>(pChar));
                        _vecIndexMiscBuffer.emplace_back(uiProgressive);

                        pChar->_iTimePeriodicTick = 0;
                    }
                    //else
                    //{
                    //    // This shouldn't happen... If it does, get rid of the entry on the list anyways,
                    //    //  it got desynchronized in some way and might be an invalid or even deleted and deallocated object!
                    //}

                }
                ++itMap;
                ++uiProgressive;
            }
            EXC_CATCHSUB("");

#ifdef _DEBUG
            g_Log.EventDebug("Done looping through char periodic ticks.\n");
#endif
        }

        {
            EXC_TRYSUB("Delete from List");
            // Erase in chunks, call erase the least times possible.
            sortedVecRemoveElementsByIndices(_mCharTickList, _vecIndexMiscBuffer);
            EXC_CATCHSUB("DeleteFromList");

            _vecIndexMiscBuffer.clear();
        }

        // Done working with _mCharTickList, we don't need the lock from now on.
    }

    {
        EXC_TRYSUB("Char Periodic Ticks Loop");
        for (void* pObjVoid : _vecGenericObjsToTick)    // Loop through all msecs stored, unless we passed the timestamp.
        {
            CChar* pChar = static_cast<CChar*>(pObjVoid);
            if (pChar->OnTickPeriodic())
            {
                AddCharTicking(pChar, false);
            }
            else
            {
                pChar->Delete(true);
            }
        }
        EXC_CATCHSUB("");

        _vecGenericObjsToTick.clear();
    }

    EXC_CATCH;
}
