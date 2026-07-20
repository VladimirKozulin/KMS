#include "winksm.h"

/*
 * Statistics collection and reporting.
 *
 * Collects metrics similar to Linux KSM's /sys/kernel/mm/ksm/ stats.
 */

void WinKSM_Stats_Init(void) {
    RtlZeroMemory(&g_Winksm.Stats, sizeof(WINKSM_STATS));
}

void WinKSM_Stats_Update(void) {
    /* Calculate derived statistics */
    double savedGb = (double)g_Winksm.Stats.SavedMemoryBytes / (1024.0 * 1024.0 * 1024.0);
    double cloudCostPerGbMonth = 3.0; /* AWS/Azure typical cost */
    g_Winksm.Stats.MonthlySavingsUsd = savedGb * cloudCostPerGbMonth;
}

void WinKSM_Stats_Get(WINKSM_STATS *OutStats) {
    KLOCK_QUEUE_HANDLE lockHandle;
    KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
    RtlCopyMemory(OutStats, &g_Winksm.Stats, sizeof(WINKSM_STATS));
    KeReleaseInStackQueuedSpinLock(&lockHandle);
    WinKSM_Stats_Update();
}
