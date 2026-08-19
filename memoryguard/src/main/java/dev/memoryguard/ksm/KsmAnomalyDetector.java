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
 * - Rapid growth of pages_sharing: indicates an attacker creating
 *   many pages with guessed content to trigger merges (memory probing /
 *   application fingerprinting).
 *
 * - High volatile/sharing ratio: in normal operation, volatile pages
 *   are < 10% of sharing. During an attack, the attacker writes to
 *   merged pages rapidly, causing the ratio to spike > 50%.
 *
 * Uses Exponential Moving Average (EMA) to build a baseline and
 * detects anomalies at > 3 standard deviations from the norm.
 */
public class KsmAnomalyDetector {
    private final KsmController ksmController;

    // EMA (Exponential Moving Average) tracking
    private double avgVolatileRate = 0;
    private double avgSharingRate = 0;
    private int prevPagesVolatile = 0;
    private int prevPagesSharing = 0;
    private int tickCount = 0;

    // Smoothing factor: 0.2 = new value has 20% weight
    private static final double ALPHA = 0.2;
    // Anomaly Threshold: a spike is flagged if current rate > baseline * ANOMALY_THRESHOLD
    // Raised to 25.0 for real microservice workloads with high concurrency
    private static final double ANOMALY_THRESHOLD = 25.0;
    // Minimum ticks before anomaly detection activates (builds baseline)
    private static final int WARMUP_TICKS = 5;
    // Minimum pages_sharing before volatile ratio check is meaningful
    private static final long MIN_SHARING_FOR_RATIO = 5000;
    // Volatile/sharing ratio that triggers an alert
    private static final double VOLATILE_RATIO_ALERT = 0.5;

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

        // Calculate rates of change since last check
        double volatileRate = Math.abs(currentVolatile - prevPagesVolatile);
        double sharingRate = Math.abs(currentSharing - prevPagesSharing);

        AnomalyReport report = null;

        if (tickCount > WARMUP_TICKS) {
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

            // --- Anomaly 2: pages_sharing spike ---
            // Rapid sharing growth means someone is creating many pages with
            // specific content to trigger merges — memory probing / fingerprinting.
            if (report == null &&
                    sharingRate > avgSharingRate * ANOMALY_THRESHOLD && avgSharingRate > 10) {
                report = AnomalyReport.alert(
                        String.format("pages_sharing spike (rate=%.0f, avg=%.0f) — " +
                                "possible memory probing/fingerprinting",
                                sharingRate, avgSharingRate),
                        currentVolatile, currentSharing, volatileRate, sharingRate);
            }

            // --- Anomaly 3: high volatile/sharing ratio ---
            // Normal: < 10%. Under attack: > 50% (attacker repeatedly writes to
            // merged pages to measure CoW latency).
            if (report == null && currentSharing > MIN_SHARING_FOR_RATIO) {
                double volatileRatio = (double) currentVolatile / currentSharing;
                if (volatileRatio > VOLATILE_RATIO_ALERT) {
                    report = AnomalyReport.alert(
                            String.format("volatile/sharing ratio=%.1f%% — " +
                                    "abnormal CoW activity detected",
                                    volatileRatio * 100),
                            currentVolatile, currentSharing, volatileRate, sharingRate);
                }
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
        avgVolatileRate = 0;
        avgSharingRate = 0;
        prevPagesVolatile = 0;
        prevPagesSharing = 0;
        tickCount = 0;
    }
}
