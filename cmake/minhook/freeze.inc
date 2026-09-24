//-------------------------------------------------------------------------
// 【段 FT (Tsukuyomi)】全スレッドの止め方を差し替えた (2026-09-24。MinHook v1.3.4 の EnumerateThreads / Freeze /
// Unfreeze の置き換え。当て方は cmake/minhook/PatchFreeze.cmake)
//
// ユーザーの記録 (1.26.51.01): FreeCamera を切ったとき、フックの群を外す MH_ApplyQueued の中で本流が 2 秒止まった
// (Freeze → ProcessThreadIPs → GetThreadContext の中)。開発 PC で測った元の作りの重さは 2 つ (tests/ft/README.md):
//
//   1. スレッドの一覧を CreateToolhelp32Snapshot で作っていた。これはシステム全体のスレッドを写すので、
//      自分のプロセスの 70〜130 本を探すために 9,166 本をたどり、1 回 70〜80 ms (呼んだ本流が止まる)。
//      → NtGetNextThread で自分のプロセスのスレッドだけを順に開く (同じ PC で 0.1 ms)。使えなければ元の形に戻す。
//   2. 1 本ずつ「SuspendThread → GetThreadContext (止まるのを待つ)」を順にやっていた。止めたスレッドは Unfreeze まで
//      止めたままなので、k 本目を待つ間、後ろのスレッドは走り続ける。しかもゲームには優先度 IDLE (-15) の
//      D3D Background Thread が 5 本あり、CPU が埋まっていると停止を受け付けるまで 1 本 2.5 秒待つことがある
//      (外から同じ順で止めて 10.3 秒を実測。CPU を埋めて 15 回: 中央値 477 ms・100 ms 以上 14 回)。
//      → 先に全部に SuspendThread を掛け (APC を積んで返るだけなので速い)、普通より低い優先度のスレッドは
//        止めている間だけ TIME_CRITICAL に上げてから、1 本ずつ GetThreadContext で止まるのを待って IP を直す
//        (同じ条件で中央値 3.9 ms・最悪 55 ms・100 ms 以上 0 回)。上げた優先度は Unfreeze が再開の前に必ず戻す。
//
// 止めたスレッドは、停止の APC を処理する前にユーザーのコードへ戻ることは無い (核の APC は戻る前に走る) ので、
// 上げた優先度で走るのは「止まる」までの核の中の仕事だけ。書き換えるのは今までどおり全部が止まった後
// (2 段目の GetThreadContext が全部返った後)。止める相手を減らすことはしない (Tsukuyomi 自身のスレッドも
// ゲームのコードのページを書き換えるので、書き換えの最中に走らせない)。
//-------------------------------------------------------------------------

// 列挙で開く権限 (元の THREAD_ACCESS + 優先度を読む・変える + スレッド ID を読む)。
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

// 開いたハンドルを全部閉じて空にする (止める前・止めずに諦めるときだけ使う。止めた後は Unfreeze)。
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

// 自分のプロセスのスレッドだけを NtGetNextThread で順に引き、スレッド ID から止めるための権限で開き直す
// (呼んだスレッドは除く)。**たどる方は最小の権限 (ID を読むだけ) で開く** — 止めるための権限でたどると、1 本でも
// 開けないスレッドがあった時点で列挙ごと失敗して元の遅い形へ戻ってしまう (Sol の二次レビュー)。開き直せない
// スレッドは元の MinHook と同じく止めない。途中で失敗したら FALSE (呼び手が集めた分を閉じて、元の形で数え直す)。
static BOOL EnumerateThreadsNt(PFROZEN_THREADS pThreads)
{
    static NTGETNEXTTHREAD s_next = NULL;
    static BOOL s_looked = FALSE;
    HANDLE hCursor = NULL;
    DWORD self = GetCurrentThreadId();

    // Freeze は MinHook のスピン錠の中からしか呼ばれないので、ここは 1 本ずつしか来ない。
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
        // 前の位置のハンドルは、次を引いた後でないと閉じられない (引くときの目印に使うため)。
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

// 元の形 (システム全体のスナップショット)。NtGetNextThread が使えないときだけ。
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
                    // 開けないスレッドは元の作りと同じく止めない (元は Freeze の中で開いて、失敗したら飛ばしていた)。
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

//-------------------------------------------------------------------------
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

    // 1 段目: 全部に停止を頼む。SuspendThread は停止の APC を積んで返るだけなので、走っているスレッドもすぐ止まり、
    // 待つのは 2 段目でまとめて (1 本ずつ待つと、まだ頼んでいないスレッドが CPU を使い続ける)。
    for (i = 0; i < pThreads->size; ++i)
    {
        PFROZEN_THREAD pItem = &pThreads->pItems[i];
        int priority;
        if (SuspendThread(pItem->hThread) == 0xFFFFFFFF)
            continue;
        pItem->suspended = TRUE;

        // 普通より低い優先度のスレッドは、CPU が埋まっていると停止の APC を処理するまで秒単位で待たされる。
        // 止めている間だけ上げる (戻すのは Unfreeze。再開より前)。**上げるのは、どの優先度クラスでも有効な 3 つの値の
        // ときだけ** — ほかの値 (実時間クラスでだけ有効な -7〜-3 など) は、止めている間にクラスが変わると戻せなくなる
        // (Sol の二次レビュー)。
        priority = GetThreadPriority(pItem->hThread);
        if ((priority == THREAD_PRIORITY_IDLE || priority == THREAD_PRIORITY_LOWEST
             || priority == THREAD_PRIORITY_BELOW_NORMAL)
            && SetThreadPriority(pItem->hThread, THREAD_PRIORITY_TIME_CRITICAL))
        {
            pItem->boosted  = TRUE;
            pItem->priority = priority;
        }
    }

    // 2 段目: 止まるのを 1 本ずつ待ち (GetThreadContext は相手が止まって文脈を返すまで待つ)、IP を直す。
    for (i = 0; i < pThreads->size; ++i)
    {
        if (pThreads->pItems[i].suspended)
            ProcessThreadIPs(pThreads->pItems[i].hThread, pos, action);
    }

    return MH_OK;
}

//-------------------------------------------------------------------------
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
                // 上げた優先度は再開の前に戻す (上げたまま走らせない)。戻せなかったら、せめて普通まで下げる。
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

