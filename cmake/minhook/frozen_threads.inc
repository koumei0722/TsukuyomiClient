// Suspended threads for Freeze()/Unfreeze().
//
// 【段 FT (Tsukuyomi)】スレッド ID ではなく開いたハンドルを持つ (列挙で開いたものを Unfreeze まで持ち、閉じるのは
// Unfreeze)。止めている間だけ優先度を上げたスレッドは、戻す値も持つ。注記は cmake/minhook/freeze.inc。
typedef struct _FROZEN_THREAD
{
    HANDLE hThread;         // FREEZE_THREAD_ACCESS で開いたハンドル
    BOOL   suspended;       // SuspendThread が通った (Unfreeze で再開する)
    BOOL   boosted;         // 止めている間だけ優先度を上げた (Unfreeze で再開の前に戻す)
    int    priority;        // 戻す優先度
} FROZEN_THREAD, *PFROZEN_THREAD;

typedef struct _FROZEN_THREADS
{
    PFROZEN_THREAD pItems;  // Data heap
    UINT    capacity;       // Size of allocated data heap, items
    UINT    size;           // Actual number of data items
} FROZEN_THREADS, *PFROZEN_THREADS;
