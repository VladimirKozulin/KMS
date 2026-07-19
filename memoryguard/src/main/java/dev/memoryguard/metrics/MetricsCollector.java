package dev.memoryguard.metrics;

import dev.memoryguard.ksm.KsmController;
import dev.memoryguard.ksm.KsmTuner;
import dev.memoryguard.containers.DockerClient;
import java.util.List;

public class MetricsCollector {
    private final KsmController ksmController;
    private final KsmTuner ksmTuner;
    private final DockerClient dockerClient;

    public MetricsCollector(KsmController ksmController, KsmTuner ksmTuner, DockerClient dockerClient) {
        this.ksmController = ksmController;
        this.ksmTuner = ksmTuner;
        this.dockerClient = dockerClient;
    }

    public static class MetricsReport {
        public boolean ksmSupported;
        public boolean ksmRunning;
        public int pagesShared;
        public int pagesSharing;
        public int pagesUnshared;
        public int pagesVolatile;
        public int fullScans;
        public int sleepMillisecs;
        public int pagesToScan;
        public boolean useZeroPages;
        public boolean autoTune;

        public long systemTotalMemory;
        public long systemAvailableMemory;
        public double systemMemoryUsagePercent;

        public double savedMemoryMb;
        public double monthlySavingsUsd;

        public List<DockerClient.ContainerInfo> containers;
    }

    public MetricsReport collect() {
        MetricsReport report = new MetricsReport();
        report.ksmSupported = ksmController.isKsmSupported();
        report.ksmRunning = ksmController.isRunning();
        report.pagesShared = ksmController.getPagesShared();
        report.pagesSharing = ksmController.getPagesSharing();
        report.pagesUnshared = ksmController.getPagesUnshared();
        report.pagesVolatile = ksmController.getPagesVolatile();
        report.fullScans = ksmController.getFullScans();
        report.sleepMillisecs = ksmController.getSleepMillisecs();
        report.pagesToScan = ksmController.getPagesToScan();
        report.useZeroPages = ksmController.isUseZeroPages();
        report.autoTune = ksmTuner.isAutoTune();

        long[] systemMem = KsmTuner.getSystemMemoryInfo();
        if (systemMem != null) {
            report.systemTotalMemory = systemMem[0];
            report.systemAvailableMemory = systemMem[1];
            long used = systemMem[0] - systemMem[1];
            report.systemMemoryUsagePercent = (double) used / systemMem[0] * 100.0;
        }

        // net saved = pages_sharing * 4096 bytes
        long savedBytes = (long) report.pagesSharing * 4096;
        report.savedMemoryMb = savedBytes / 1024.0 / 1024.0;

        double cloudCostPerGbMonth = 3.0;
        double savedGb = report.savedMemoryMb / 1024.0;
        report.monthlySavingsUsd = savedGb * cloudCostPerGbMonth;

        report.containers = dockerClient.getRunningContainers();

        return report;
    }
}
