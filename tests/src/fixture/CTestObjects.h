#ifndef _INC_CTESTOBJECTS_H
#define _INC_CTESTOBJECTS_H

#include "../CWorldTestAccess.h"
#include "../../../src/common/CUID.h"
#include "../../../src/game/CObjBase.h"
#include <vector>

// The objects a test creates, deleted at the end of the scope as the server does: Delete (with what they carry), then
//  the end of the tick destroys them, with the other objects scheduled for deletion. Those already destroyed are skipped.
class CTestObjects
{
    std::vector<CUID> _vObjs;

public:
    CTestObjects() = default;
    CTestObjects(const CTestObjects&) = delete;
    CTestObjects& operator=(const CTestObjects&) = delete;

    ~CTestObjects()
    {
        for (const CUID& uid : _vObjs)
        {
            if (CObjBase* pObj = uid.ObjFind(true))
                pObj->Delete(true);
        }
        CWorldTestAccess::DestroyScheduledDeletions();
    }

    template <class T>
    T* Keep(T* pObj)
    {
        if (pObj != nullptr)
            _vObjs.push_back(pObj->GetUID());
        return pObj;
    }
};

#endif // _INC_CTESTOBJECTS_H
