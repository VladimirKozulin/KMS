package dev.memoryguard.ksm;

/**
 * Feedback controller for the ksmd scan rate. Pure logic, no I/O.
 *
 * The threshold tuner chose a speed from free memory alone, which in
 * experiment 3 meant: maximum speed for hours under load while savings sat on
 * a plateau (scanning GC'd heaps that never merge), and slowing down exactly
 * when an idle host was gaining gigabytes (savings freed memory → "less
 * pressure" → slower). This controller looks at what scanning actually buys:
 *
 *  - CONVERGE: after start, an unmerge or newly deployed processes, scan at
 *    the pressure ceiling until ksmd has made
 *    CONVERGE_FULL_SCANS full passes — KSM merges a page only when it sees it
 *    unchanged on a 2nd pass, so efficiency is meaningless before that.
 *  - TRACK, over a sliding window of WINDOW ticks (~2 min):
 *      net   = (saved now − saved then) / pages scanned
 *      gross = sum of per-tick increases / pages scanned
 *    · net ≥ EFF_HIGH  → savings still growing: speed up ×1.5
 *    · gross < EFF_LOW → nothing merges any more: back off ×0.6 to the floor
 *    · otherwise       → churn: the workload breaks merged pages and KSM
 *      re-merges others; net stays flat. Re-merges are real but short-lived,
 *      so glide (×0.85) down to a maintenance rate = MAINTENANCE_SHARE of
 *      the ceiling instead of paying full price for them.
 *    Rates are clamped to [floor, ceiling] of the current memory pressure.
 *
 * Why a window and net/gross: in experiment 3 the converged phases under load
 * had net efficiency 0.0000 while per-tick gains alone looked like 0.006–0.012
 * (merged pages broken and re-merged) — a per-tick, losses-clamped measure
 * would have kept ksmd at full speed forever.
 *
 * Merged pages stay merged without rescanning, so backing off keeps the
 * savings already achieved and only stops paying for scans that find nothing.
 */
public final class AdaptiveScanPolicy {
    public enum Mode { CONVERGE, TRACK }

    /** One observation of KSM counters. scannedPages = -1 if the kernel lacks pages_scanned. */
    public record Sample(double freeRatio, long savedPages, long scannedPages, long fullScans) {}

    public record Decision(Mode mode, double ratePagesPerSec, double efficiency, String reason) {}

    static final int CONVERGE_FULL_SCANS = 2;
    /** ≥ 1 page saved per 100 scanned: scanning pays off, go faster */
    static final double EFF_HIGH = 0.01;
    /** < 1 page saved per 1000 scanned: not worth the CPU, slow down */
    static final double EFF_LOW = 0.001;
    // No "free memory fell" trigger: in experiment 3 (compare run A1) it fired
    // whenever load started — growth that is garbage, not mergeable data — and
    // forced full speed for minutes. TRACK speeds up by itself if new memory
    // does start to merge.
    /** Sliding window for efficiency, in ticks (12–20 s each) */
    static final int WINDOW = 6;
    /** Rate kept while the workload churns merged pages, as a share of the ceiling */
    static final double MAINTENANCE_SHARE = 0.2;
    /** Ignore deployments smaller than this */
    static final long MIN_NEW_MEMORY_BYTES = 64L << 20;

    /** Highest scan rate (pages/s) worth paying for at this pressure. */
    static double ceiling(double free) {
        if (free < 0.10) return 100_000;
        if (free < 0.20) return 50_000;
        if (free < 0.50) return 20_000;
        return 10_000;
    }

    /** Background rate that still notices slow changes. */
    static double floor(double free) {
        if (free < 0.10) return 4_000;
        if (free < 0.20) return 2_000;
        if (free < 0.50) return 1_000;
        return 500;
    }

    private Mode mode = Mode.CONVERGE;
    private long convergeStartScans = -1;
    private double rate = 0;
    private final java.util.ArrayDeque<Sample> window = new java.util.ArrayDeque<>();
    private String pendingEvent = "start";

    public synchronized void notifyNewMemory(long bytes) {
        if (bytes >= MIN_NEW_MEMORY_BYTES && pendingEvent == null) {
            pendingEvent = "new mergeable memory " + (bytes >> 20) + " MiB";
        }
    }

    public synchronized void notifyUnmerge() {
        pendingEvent = "after unmerge";
    }

    public synchronized Mode mode() {
        return mode;
    }

    public synchronized Decision decide(Sample s) {
        window.addLast(s);
        while (window.size() > WINDOW + 1) window.removeFirst();
        double net = Double.NaN, gross = Double.NaN;
        Sample first = window.peekFirst();
        if (window.size() > 2 && s.scannedPages() >= 0 && first.scannedPages() >= 0) {
            long dScan = s.scannedPages() - first.scannedPages();
            if (dScan > 0) {
                long grossGain = 0;
                Sample prev = null;
                for (Sample w : window) {
                    if (prev != null) grossGain += Math.max(0, w.savedPages() - prev.savedPages());
                    prev = w;
                }
                net = Math.max(0, s.savedPages() - first.savedPages()) / (double) dScan;
                gross = grossGain / (double) dScan;
            }
        }

        double ceil = ceiling(s.freeRatio());
        double flo = floor(s.freeRatio());

        if (pendingEvent != null) {
            mode = Mode.CONVERGE;
            convergeStartScans = s.fullScans();
            String reason = "converge: " + pendingEvent;
            pendingEvent = null;
            window.clear();
            window.addLast(s);
            rate = ceil;
            return new Decision(mode, rate, net, reason);
        }

        if (mode == Mode.CONVERGE) {
            long done = s.fullScans() - convergeStartScans;
            if (done < CONVERGE_FULL_SCANS) {
                rate = ceil;
                return new Decision(mode, rate, net, "converge: full scans " + done + "/" + CONVERGE_FULL_SCANS);
            }
            mode = Mode.TRACK;
            window.clear();      // judge TRACK on post-convergence data only
            window.addLast(s);
        }

        String reason;
        if (rate <= 0) rate = flo;
        double maintenance = Math.max(flo, ceil * MAINTENANCE_SHARE);
        if (Double.isNaN(net)) {
            reason = "track: collecting data, hold";
        } else if (net >= EFF_HIGH) {
            rate *= 1.5;
            reason = String.format(java.util.Locale.ROOT, "track: net %.4f >= %.3f, savings growing, speed up", net, EFF_HIGH);
        } else if (gross < EFF_LOW) {
            rate *= 0.6;
            reason = String.format(java.util.Locale.ROOT, "track: gross %.5f < %.3f, nothing to merge, back off", gross, EFF_LOW);
        } else if (rate > maintenance) {
            rate = Math.max(maintenance, rate * 0.85);
            reason = String.format(java.util.Locale.ROOT, "track: net %.4f, gross %.4f, churn, glide to maintenance", net, gross);
        } else {
            reason = String.format(java.util.Locale.ROOT, "track: net %.4f, gross %.4f, churn, hold", net, gross);
        }
        rate = Math.max(flo, Math.min(ceil, rate));
        return new Decision(mode, rate, net, reason);
    }

    /**
     * Converts a scan rate into ksmd knobs {pages_to_scan, sleep_millisecs}
     * (before anti-timing jitter): ~20 ms batches, longer sleeps for tiny
     * rates, shorter sleeps instead of huge batches for high ones.
     */
    static int[] toKnobs(double ratePagesPerSec) {
        int sleep = 20;
        long pages = Math.round(ratePagesPerSec * sleep / 1000.0);
        if (pages < 32) {
            pages = 32;
            sleep = (int) Math.min(1000, Math.round(32 * 1000.0 / Math.max(1, ratePagesPerSec)));
        } else if (pages > 2000) {
            sleep = 10;
            pages = Math.min(4000, Math.round(ratePagesPerSec * sleep / 1000.0));
        }
        return new int[]{(int) pages, sleep};
    }
}
