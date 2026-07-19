package dev.memoryguard.ksm;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

public class KsmTuner implements Runnable {
    private final KsmController ksmController;
    private boolean autoTune = true;
    private volatile boolean running = true;

    public KsmTuner(KsmController ksmController) {
        this.ksmController = ksmController;
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
        while (running) {
            if (autoTune && ksmController.isRunning()) {
                tune();
            }
            try {
                Thread.sleep(15000); // Check and tune every 15 seconds
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
    }

    private void tune() {
        try {
            long[] memInfo = getSystemMemoryInfo();
            if (memInfo == null) return;

            long total = memInfo[0];
            long available = memInfo[1];
            double freePercentage = (double) available / total * 100.0;

            // Simple adaptive logic:
            // High memory pressure (available memory < 20%): scan aggressively
            if (freePercentage < 20.0) {
                ksmController.setPagesToScan(500);
                ksmController.setSleepMillisecs(10);
            }
            // Moderate pressure (20% - 50%): scan moderately
            else if (freePercentage < 50.0) {
                ksmController.setPagesToScan(200);
                ksmController.setSleepMillisecs(20);
            }
            // Low pressure (> 50%): scan slowly to save CPU
            else {
                ksmController.setPagesToScan(100);
                ksmController.setSleepMillisecs(50);
            }
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
