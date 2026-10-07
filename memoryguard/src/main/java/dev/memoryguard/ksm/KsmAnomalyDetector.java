package dev.memoryguard.ksm;

/**
 * KSM Side-Channel Anomaly Detector.
 *
 * Monitors KSM kernel metrics for patterns characteristic of active
 * side-channel attacks:
 *
 * - Rapid growth of pages_volatile: indicates mass CoW faults,
 *   typically caused by an attacker writing to merged pages to
 *   measure timing differences (CoW timing attack).
 *
 * - Mass drop of pages_sharing: many merged pages broken by writes in one
 *   tick — the CoW-probe pattern (merge, then write to time the fault).
 *
 * NOT a signal: growth of pages_sharing. That is KSM doing its job — after
 * start, after every unmerge and whenever new replicas are deployed. A probing
 * attacker produces one merge per correct guess, not 100k-page jumps. The
 * first version alerted on growth and froze ksmd on its own convergence
 * (experiment 3, run 1: 2026-10-07 11:50:40).
 *
 * Removed: the volatile/sharing ratio rule. With use_zero_pages=1 the savings
 * are counted in ksm_zero_pages, not pages_sharing, while pages_volatile is in
 * the millions on any GC'd heap — the ratio hit 61997% on a healthy host
 * (experiment 3, run 2) and froze ksmd.
 *
 * Limits: these are host-wide counters. They cannot attribute activity to a
 * container nor separate a careful attacker from GC churn, so by default the
 * tuner only LOGS anomalies (MEMORYGUARD_ANOMALY_ACTION=respond to act on them).
 *
 * Uses Exponential Moving Average (EMA) to build a baseline; a rate above
 * ANOMALY_THRESHOLD × baseline is flagged.
 */
public class KsmAnomalyDetector {
    private final KsmController ksmController;

    // EMA (Exponential Moving Average) tracking
    private double avgVolatileRate = 0;
    private double avgSharingRate = 0;
    private int prevPagesVolatile = 0;
    private int prevPagesSharing = 0;
    private int tickCount = 0;
    // full_scans value when the detector was (re)armed; -1 = not yet read
    private int armedAtFullScans = -1;

    // Smoothing factor: 0.2 = new value has 20% weight
    private static final double ALPHA = 0.2;
    // Anomaly Threshold: a spike is flagged if current rate > baseline * ANOMALY_THRESHOLD
    // Raised to 25.0 for real microservice workloads with high concurrency
    private static final double ANOMALY_THRESHOLD = 25.0;
    // Minimum ticks before anomaly detection activates (builds baseline)
    private static final int WARMUP_TICKS = 5;
    // KSM merges a page only on its 2nd stable encounter, so the first full
    // scans after start/unmerge are a burst of legitimate merges. Stay silent
    // until this many full scans have completed since (re)arming.
    static final int CONVERGENCE_FULL_SCANS = 3;
    // Fraction of merged pages broken within one tick that counts as mass CoW
    private static final double MASS_UNMERGE_FRACTION = 0.25;
    // Minimum pages_sharing before the mass-drop rule is meaningful
    private static final long MIN_SHARING_FOR_RATIO = 5000;

    public KsmAnomalyDetector(KsmController ksmController) {
        this.ksmController = ksmController;
    }

    /**
     * Immutable report from a single anomaly check.
     */
    public static class AnomalyReport {
        public final boolean suspicious;
        public final String reason;
        public final int currentVolatile;
        public final int currentSharing;
        public final double volatileRate;
        public final double sharingRate;

        private AnomalyReport(boolean suspicious, String reason,
                              int currentVolatile, int currentSharing,
                              double volatileRate, double sharingRate) {
            this.suspicious = suspicious;
            this.reason = reason;
            this.currentVolatile = currentVolatile;
            this.currentSharing = currentSharing;
            this.volatileRate = volatileRate;
            this.sharingRate = sharingRate;
        }

        static AnomalyReport ok(int vol, int sharing, double volRate, double shrRate) {
            return new AnomalyReport(false, null, vol, sharing, volRate, shrRate);
        }

        static AnomalyReport alert(String reason, int vol, int sharing,
                                    double volRate, double shrRate) {
            return new AnomalyReport(true, reason, vol, sharing, volRate, shrRate);
        }
    }

    /**
     * Perform one anomaly check cycle. Should be called periodically
     * (every 12-20 seconds from KsmTuner).
     *
     * @return an {@link AnomalyReport} indicating whether suspicious
     *         activity was detected.
     */
    public AnomalyReport check() {
        int currentVolatile = ksmController.getPagesVolatile();
        int currentSharing = ksmController.getPagesSharing();
        int fullScans = ksmController.getFullScans();
        if (armedAtFullScans < 0) armedAtFullScans = fullScans;
        boolean converged = fullScans - armedAtFullScans >= CONVERGENCE_FULL_SCANS;

        // Calculate rates of change since last check
        double volatileRate = Math.abs(currentVolatile - prevPagesVolatile);
        double sharingRate = Math.abs(currentSharing - prevPagesSharing);

        AnomalyReport report = null;

        if (tickCount > WARMUP_TICKS && converged) {
            // --- Anomaly 1: pages_volatile spike ---
            // A sudden increase in volatile pages means many CoW faults
            // are occurring — characteristic of a timing probe attack.
            if (volatileRate > avgVolatileRate * ANOMALY_THRESHOLD && avgVolatileRate > 0) {
                report = AnomalyReport.alert(
                        String.format("pages_volatile spike (rate=%.0f, avg=%.0f) — " +
                                "possible CoW timing attack in progress",
                                volatileRate, avgVolatileRate),
                        currentVolatile, currentSharing, volatileRate, sharingRate);
            }

            // --- Anomaly 2: mass breaking of merged pages ---
            // A large share of merged pages un-shared by writes within one tick.
            double sharingDrop = prevPagesSharing - currentSharing;
            if (report == null && prevPagesSharing > MIN_SHARING_FOR_RATIO
                    && sharingDrop > prevPagesSharing * MASS_UNMERGE_FRACTION) {
                report = AnomalyReport.alert(
                        String.format("pages_sharing dropped %.0f%% in one tick (%d -> %d) — " +
                                "mass CoW breaking of merged pages",
                                100.0 * sharingDrop / prevPagesSharing, prevPagesSharing, currentSharing),
                        currentVolatile, currentSharing, volatileRate, sharingRate);
            }
        }

        // Update EMA baseline
        avgVolatileRate = ALPHA * volatileRate + (1 - ALPHA) * avgVolatileRate;
        avgSharingRate = ALPHA * sharingRate + (1 - ALPHA) * avgSharingRate;
        prevPagesVolatile = currentVolatile;
        prevPagesSharing = currentSharing;
        tickCount++;

        return report != null ? report :
                AnomalyReport.ok(currentVolatile, currentSharing, volatileRate, sharingRate);
    }

    /**
     * Reset the detector state (e.g. after an emergency unmerge).
     */
    public void reset() {
        armedAtFullScans = -1;   // re-learn: an unmerge restarts convergence
        avgVolatileRate = 0;
        avgSharingRate = 0;
        prevPagesVolatile = 0;
        prevPagesSharing = 0;
        tickCount = 0;
    }
}
