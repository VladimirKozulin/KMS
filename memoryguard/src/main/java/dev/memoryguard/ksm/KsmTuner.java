package dev.memoryguard.ksm;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.security.SecureRandom;
import java.util.List;

public class KsmTuner implements Runnable {
    private final KsmController ksmController;
    private volatile boolean autoTune = true;
    private volatile boolean running = true;

    // ============================================================
    // KSM Side-Channel Mitigation: Anti-Timing Protection
    // ============================================================
    private final SecureRandom secureRandom = new SecureRandom();
    private final KsmAnomalyDetector anomalyDetector;

    // Periodic unmerge — destroys any timing channel the attacker may have built
    private int unmergeCountdown = 0;
    private static final int UNMERGE_CYCLE_MIN = 20;  // ~5 min at 15s tick
    private static final int UNMERGE_CYCLE_MAX = 60;  // ~15 min at 15s tick

    private final boolean periodicUnmergeEnabled =
            !"false".equalsIgnoreCase(System.getenv("MEMORYGUARD_PERIODIC_UNMERGE"));

    // "log" (default): record the anomaly only. "respond": unmerge/freeze ksmd.
    // Host-wide counters gave two false positives in two load tests, each one
    // stopping deduplication for 10+ minutes — acting on them is opt-in.
    private final boolean anomalyRespond =
            "respond".equalsIgnoreCase(System.getenv("MEMORYGUARD_ANOMALY_ACTION"));

    // After an anomaly KSM stays throttled (or frozen) for this many ticks
    private static final int ANOMALY_COOLDOWN_TICKS = 40;   // ~10-13 min
    private int cooldownTicks = 0;
    private boolean frozenByAnomaly = false;
    private volatile String lastSecurityEvent = null;

    // Jitter bounds for the tuner sleep interval (milliseconds)
    private static final int TUNE_INTERVAL_MIN_MS = 12000;
    private static final int TUNE_INTERVAL_JITTER_MS = 8000;

    public KsmTuner(KsmController ksmController) {
        this(ksmController, null);
    }

    public KsmTuner(KsmController ksmController, KsmAnomalyDetector anomalyDetector) {
        this.ksmController = ksmController;
        this.anomalyDetector = anomalyDetector;
    }

    public void setAutoTune(boolean autoTune) {
        this.autoTune = autoTune;
    }

    public boolean isAutoTune() {
        return autoTune;
    }

    public void stop() {
        this.running = false;
    }

    public String getLastSecurityEvent() {
        return lastSecurityEvent;
    }

    public boolean isInAnomalyCooldown() {
        return cooldownTicks > 0;
    }

    @Override
    public void run() {
        resetUnmergeCountdown();
        while (running) {
            if (cooldownTicks > 0) {
                // Keep the post-anomaly throttle; don't let tune() undo it
                if (--cooldownTicks == 0) endCooldown();
            } else if (ksmController.isRunning()) {
                if (autoTune) {
                    tune();
                }

                // === Anomaly detection — runs regardless of autoTune ===
                if (anomalyDetector != null) {
                    KsmAnomalyDetector.AnomalyReport report = anomalyDetector.check();
                    if (report.suspicious) {
                        System.err.println("[SECURITY-WARN] " + report.reason);
                        if (anomalyRespond) {
                            respondToAnomaly(report.reason);
                        } else {
                            lastSecurityEvent = "anomaly (logged only): " + report.reason;
                        }
                    }
                }

                // Previously the countdown was initialised but never decremented,
                // so the periodic unmerge advertised in SECURITY.md never ran.
                if (periodicUnmergeEnabled && cooldownTicks == 0 && --unmergeCountdown <= 0) {
                    performPeriodicUnmerge();
                    resetUnmergeCountdown();
                }
            }
            try {
                // Randomized interval (12-20 sec) — prevents attacker synchronization
                int jitter = TUNE_INTERVAL_MIN_MS + secureRandom.nextInt(TUNE_INTERVAL_JITTER_MS);
                Thread.sleep(jitter);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
    }

    /**
     * Periodic unmerge + re-scan cycle.
     * Writes "2" to /sys/kernel/mm/ksm/run which forces the kernel to
     * unmerge ALL currently shared pages (the write is synchronous). After a
     * random pause, KSM is re-enabled. This resets any merge state an attacker
     * has observed, forcing them to restart their measurement.
     *
     * Skipped when the host could not hold every page unmerged — an unmerge
     * storm on a KSM-overcommitted host ends in the OOM killer.
     */
    private void performPeriodicUnmerge() {
        if (!ksmController.canAffordUnmerge()) {
            System.out.println("[SECURITY] Periodic unmerge skipped: needs "
                    + (ksmController.unmergeCostBytes() >> 20) + " MB, not enough headroom (OOM risk)");
            return;
        }
        try {
            ksmController.writeString("run", "2");
            Thread.sleep(500 + secureRandom.nextInt(500));
            ksmController.setRunning(true);
            if (anomalyDetector != null) anomalyDetector.reset();   // counters jumped, rebuild baseline
            System.out.println("[SECURITY] Periodic unmerge completed — timing channels disrupted");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (Exception e) {
            System.err.println("[SECURITY] Unmerge cycle failed: " + e.getMessage());
        }
    }

    /**
     * Emergency response to a detected anomaly.
     *  - enough memory headroom → unmerge everything, restart KSM throttled
     *    (sleep=200ms, pages_to_scan=50) so new probes take far longer
     *  - not enough headroom   → freeze ksmd (run=0): no NEW merges happen, so
     *    an attacker's probe pages can no longer merge with victim pages, and
     *    the savings already achieved are kept
     * Either way the state is held for ANOMALY_COOLDOWN_TICKS.
     */
    private void respondToAnomaly(String reason) {
        try {
            if (ksmController.canAffordUnmerge()) {
                ksmController.writeString("run", "2");
                ksmController.setSleepMillisecs(200);
                ksmController.setPagesToScan(50);
                ksmController.setRunning(true);
                frozenByAnomaly = false;
                lastSecurityEvent = "anomaly: unmerged + throttled (" + reason + ")";
            } else {
                ksmController.setRunning(false);
                frozenByAnomaly = true;
                lastSecurityEvent = "anomaly: ksmd frozen, unmerge unaffordable (" + reason + ")";
            }
            System.err.println("[SECURITY] " + lastSecurityEvent);
        } catch (IOException e) {
            System.err.println("[SECURITY] Emergency response failed: " + e.getMessage());
        }
        cooldownTicks = ANOMALY_COOLDOWN_TICKS;
        if (anomalyDetector != null) anomalyDetector.reset();
    }

    private void endCooldown() {
        if (frozenByAnomaly) {
            try {
                ksmController.setRunning(true);
                System.out.println("[SECURITY] Anomaly cooldown over — ksmd resumed");
            } catch (IOException e) {
                System.err.println("[SECURITY] Could not resume ksmd: " + e.getMessage());
            }
            frozenByAnomaly = false;
        }
        resetUnmergeCountdown();
    }

    private void resetUnmergeCountdown() {
        unmergeCountdown = UNMERGE_CYCLE_MIN
                + secureRandom.nextInt(UNMERGE_CYCLE_MAX - UNMERGE_CYCLE_MIN);
    }

    private void tune() {
        try {
            long[] memInfo = getSystemMemoryInfo();
            if (memInfo == null) return;

            long total = memInfo[0];
            long available = memInfo[1];
            double freePercentage = (double) available / total * 100.0;

            // Base parameters for each pressure level
            int baseScan;
            int baseSleep;

            // High memory pressure (available memory < 20%): scan aggressively
            if (freePercentage < 20.0) {
                baseScan = 500;
                baseSleep = 10;
            }
            // Moderate pressure (20% - 50%): scan moderately
            else if (freePercentage < 50.0) {
                baseScan = 200;
                baseSleep = 20;
            }
            // Low pressure (> 50%): scan slowly to save CPU
            else {
                baseScan = 100;
                baseSleep = 50;
            }

            // === Anti-timing: add ±30% jitter to KSM parameters ===
            int scanJitter = Math.max(1, (int) (baseScan * 0.3));
            int sleepJitter = Math.max(1, (int) (baseSleep * 0.3));

            int actualScan = baseScan - scanJitter + secureRandom.nextInt(scanJitter * 2 + 1);
            int actualSleep = baseSleep - sleepJitter + secureRandom.nextInt(sleepJitter * 2 + 1);

            ksmController.setPagesToScan(Math.max(50, actualScan));
            ksmController.setSleepMillisecs(Math.max(5, actualSleep));
        } catch (Exception e) {
            // Ignore tuning errors in background
        }
    }

    public static long[] getSystemMemoryInfo() {
        try {
            List<String> lines = Files.readAllLines(Paths.get("/proc/meminfo"));
            long total = 0;
            long available = 0;
            for (String line : lines) {
                if (line.startsWith("MemTotal:")) {
                    total = parseMemLine(line);
                } else if (line.startsWith("MemAvailable:")) {
                    available = parseMemLine(line);
                }
            }
            if (total > 0 && available > 0) {
                return new long[]{total, available};
            }
        } catch (IOException e) {
            // Probably not on Linux
        }
        return null;
    }

    private static long parseMemLine(String line) {
        String parts = line.replaceAll("[^0-9]", "");
        try {
            return Long.parseLong(parts) * 1024; // Convert kB to bytes
        } catch (NumberFormatException e) {
            return 0;
        }
    }
}
