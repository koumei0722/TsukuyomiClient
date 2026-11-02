#define FREEZE_THREAD_ACCESS \
    (THREAD_ACCESS | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION)

typedef LONG (NTAPI *NTGETNEXTTHREAD)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

#define FREEZE_STATUS_SUCCESS         ((LONG)0x00000000L)
#define FREEZE_STATUS_NO_MORE_ENTRIES ((LONG)0x8000001AL)

static BOOL AddFrozenThread(PFROZEN_THREADS pThreads, HANDLE hThread)
{
    PFROZEN_THREAD pItem;
    if (pThreads->pItems == NULL)
    {
        pThreads->capacity = INITIAL_THREAD_CAPACITY;
        pThreads->pItems
            = (PFROZEN_THREAD)HeapAlloc(g_hHeap, 0, pThreads->capacity * sizeof(FROZEN_THREAD));
        if (pThreads->pItems == NULL)
            return FALSE;
    }
    else if (pThreads->size >= pThreads->capacity)
    {
        PFROZEN_THREAD p;
        pThreads->capacity *= 2;
        p = (PFROZEN_THREAD)HeapReAlloc(
            g_hHeap, 0, pThreads->pItems, pThreads->capacity * sizeof(FROZEN_THREAD));
        if (p == NULL)
            return FALSE;

        pThreads->pItems = p;
    }
    pItem = &pThreads->pItems[pThreads->size++];
    pItem->hThread   = hThread;
    pItem->suspended = FALSE;
    pItem->boosted   = FALSE;
    pItem->priority  = THREAD_PRIORITY_NORMAL;
    return TRUE;
}

static VOID ReleaseFrozenThreads(PFROZEN_THREADS pThreads)
{
    UINT i;
    if (pThreads->pItems != NULL)
    {
        for (i = 0; i < pThreads->size; ++i)
            CloseHandle(pThreads->pItems[i].hThread);
        HeapFree(g_hHeap, 0, pThreads->pItems);
    }
    pThreads->pItems   = NULL;
    pThreads->capacity = 0;
    pThreads->size     = 0;
}

static BOOL EnumerateThreadsNt(PFROZEN_THREADS pThreads)
{
    static NTGETNEXTTHREAD s_next = NULL;
    static BOOL s_looked = FALSE;
    HANDLE hCursor = NULL;
    DWORD self = GetCurrentThreadId();

    if (!s_looked)
    {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll != NULL)
            s_next = (NTGETNEXTTHREAD)(void*)GetProcAddress(hNtdll, "NtGetNextThread");
        s_looked = TRUE;
    }
    if (s_next == NULL)
        return FALSE;

    for (;;)
    {
        HANDLE hNext = NULL;
        HANDLE hThread;
        DWORD tid;
        LONG status = s_next(GetCurrentProcess(), hCursor, THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &hNext);
        if (hCursor != NULL)
            CloseHandle(hCursor);
        if (status != FREEZE_STATUS_SUCCESS)
            return status == FREEZE_STATUS_NO_MORE_ENTRIES;

        hCursor = hNext;
        tid = GetThreadId(hNext);
        if (tid == 0 || tid == self)
            continue;
        hThread = OpenThread(FREEZE_THREAD_ACCESS, FALSE, tid);
        if (hThread == NULL)
            continue;
        if (!AddFrozenThread(pThreads, hThread))
        {
            CloseHandle(hThread);
            CloseHandle(hCursor);
            return FALSE;
        }
    }
}

static BOOL EnumerateThreadsToolhelp(PFROZEN_THREADS pThreads)
{
    BOOL succeeded = FALSE;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te;
        te.dwSize = sizeof(THREADENTRY32);
        if (Thread32First(hSnapshot, &te))
        {
            succeeded = TRUE;
            do
            {
                if (te.dwSize >= (FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD))
                    && te.th32OwnerProcessID == GetCurrentProcessId()
                    && te.th32ThreadID != GetCurrentThreadId())
                {
                    HANDLE hThread = OpenThread(FREEZE_THREAD_ACCESS, FALSE, te.th32ThreadID);
                    if (hThread != NULL && !AddFrozenThread(pThreads, hThread))
                    {
                        CloseHandle(hThread);
                        succeeded = FALSE;
                        break;
                    }
                }

                te.dwSize = sizeof(THREADENTRY32);
            } while (Thread32Next(hSnapshot, &te));

            if (succeeded && GetLastError() != ERROR_NO_MORE_FILES)
                succeeded = FALSE;
        }
        CloseHandle(hSnapshot);
    }

    return succeeded;
}

static MH_STATUS Freeze(PFROZEN_THREADS pThreads, UINT pos, UINT action)
{
    UINT i;

    pThreads->pItems   = NULL;
    pThreads->capacity = 0;
    pThreads->size     = 0;
    if (!EnumerateThreadsNt(pThreads))
    {
        ReleaseFrozenThreads(pThreads);
        if (!EnumerateThreadsToolhelp(pThreads))
        {
            ReleaseFrozenThreads(pThreads);
            return MH_ERROR_MEMORY_ALLOC;
        }
    }

    for (i = 0; i < pThreads->size; ++i)
    {
        PFROZEN_THREAD pItem = &pThreads->pItems[i];
        int priority;
        if (SuspendThread(pItem->hThread) == 0xFFFFFFFF)
            continue;
        pItem->suspended = TRUE;

        priority = GetThreadPriority(pItem->hThread);
        if ((priority == THREAD_PRIORITY_IDLE || priority == THREAD_PRIORITY_LOWEST
             || priority == THREAD_PRIORITY_BELOW_NORMAL)
            && SetThreadPriority(pItem->hThread, THREAD_PRIORITY_TIME_CRITICAL))
        {
            pItem->boosted  = TRUE;
            pItem->priority = priority;
        }
    }

    for (i = 0; i < pThreads->size; ++i)
    {
        if (pThreads->pItems[i].suspended)
            ProcessThreadIPs(pThreads->pItems[i].hThread, pos, action);
    }

    return MH_OK;
}

static VOID Unfreeze(PFROZEN_THREADS pThreads)
{
    if (pThreads->pItems != NULL)
    {
        UINT i;
        for (i = 0; i < pThreads->size; ++i)
        {
            PFROZEN_THREAD pItem = &pThreads->pItems[i];
            if (pItem->suspended)
            {
                if (pItem->boosted && !SetThreadPriority(pItem->hThread, pItem->priority))
                    SetThreadPriority(pItem->hThread, THREAD_PRIORITY_NORMAL);
                ResumeThread(pItem->hThread);
            }
            CloseHandle(pItem->hThread);
        }

        HeapFree(g_hHeap, 0, pThreads->pItems);
        pThreads->pItems   = NULL;
        pThreads->capacity = 0;
        pThreads->size     = 0;
    }
}
