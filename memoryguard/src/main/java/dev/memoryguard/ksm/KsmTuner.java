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

    @Override
    public void run() {
        resetUnmergeCountdown();
        while (running) {
            if (autoTune && ksmController.isRunning()) {
                tune();

                // === Anomaly detection ===
                if (anomalyDetector != null) {
                    KsmAnomalyDetector.AnomalyReport report = anomalyDetector.check();
                    if (report.suspicious) {
                        System.err.println("[SECURITY-WARN] " + report.reason);
                    }
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
     * unmerge ALL currently shared pages. After a random pause, KSM is
     * re-enabled. This destroys any timing channel an attacker has built,
     * forcing them to restart their measurement — but the next scan cycle
     * will use different intervals due to randomization.
     */
    private void performPeriodicUnmerge() {
        try {
            // run=2 → unmerge all currently shared pages
            ksmController.writeString("run", "2");
            // Random pause before re-enabling (0.5 - 1.0 sec)
            Thread.sleep(500 + secureRandom.nextInt(500));
            // run=1 → restart KSM scanning
            ksmController.setRunning(true);
            System.out.println("[SECURITY] Periodic unmerge completed — timing channels disrupted");
        } catch (Exception e) {
            System.err.println("[SECURITY] Unmerge cycle failed: " + e.getMessage());
        }
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
