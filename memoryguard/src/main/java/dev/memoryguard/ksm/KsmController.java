package dev.memoryguard.ksm;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public class KsmController {
    private static final Path KSM_DIR = Paths.get("/sys/kernel/mm/ksm");

    public boolean isKsmSupported() {
        return Files.exists(KSM_DIR);
    }

    public boolean isRunning() {
        try {
            return readString("run").equals("1");
        } catch (IOException e) {
            return false;
        }
    }

    public void setRunning(boolean run) throws IOException {
        writeString("run", run ? "1" : "0");
    }

    public int getPagesShared() {
        return readIntSafe("pages_shared");
    }

    public int getPagesSharing() {
        return readIntSafe("pages_sharing");
    }

    public int getPagesUnshared() {
        return readIntSafe("pages_unshared");
    }

    public int getPagesVolatile() {
        return readIntSafe("pages_volatile");
    }

    public int getFullScans() {
        return readIntSafe("full_scans");
    }

    public int getSleepMillisecs() {
        return readIntSafe("sleep_millisecs");
    }

    public void setSleepMillisecs(int ms) throws IOException {
        writeString("sleep_millisecs", String.valueOf(ms));
    }

    public int getPagesToScan() {
        return readIntSafe("pages_to_scan");
    }

    public void setPagesToScan(int pages) throws IOException {
        writeString("pages_to_scan", String.valueOf(pages));
    }

    public boolean isUseZeroPages() {
        return readIntSafe("use_zero_pages") == 1;
    }

    public void setUseZeroPages(boolean use) throws IOException {
        writeString("use_zero_pages", use ? "1" : "0");
    }

    // ============================================================
    // KSM Side-Channel Mitigation: Security Hardening Controls
    // ============================================================

    /**
     * max_page_sharing — maximum virtual pages sharing a single physical KSM page.
     * Lower values mitigate CVE-2024-0564 (timing side-channel via unmap latency).
     * Kernel default: 256. Secure value: 2-4.
     */
    public int getMaxPageSharing() {
        return readIntSafe("max_page_sharing");
    }

    public void setMaxPageSharing(int max) throws IOException {
        if (max < 1 || max > 256) {
            throw new IllegalArgumentException("max_page_sharing must be 1-256, got: " + max);
        }
        writeString("max_page_sharing", String.valueOf(max));
    }

    /**
     * merge_across_nodes — whether to merge pages across NUMA nodes.
     * Disabling restricts merging to same NUMA node → reduces cross-node
     * side-channel attack surface and avoids NUMA performance penalties.
     */
    public boolean isMergeAcrossNodes() {
        return readIntSafe("merge_across_nodes") == 1;
    }

    public void setMergeAcrossNodes(boolean merge) throws IOException {
        writeString("merge_across_nodes", merge ? "1" : "0");
    }

    /**
     * stable_node_chains — number of KSM stable node chains.
     * Used for monitoring deduplication tree depth.
     */
    public int getStableNodeChains() {
        return readIntSafe("stable_node_chains");
    }

    /**
     * Applies security hardening parameters on startup.
     * Called from Main.java during initialization.
     */
    public void applySecurityHardening() {
        try {
            // Limit page sharing to 2 — minimizes CVE-2024-0564 timing signal
            setMaxPageSharing(2);
            System.out.println("[SECURITY] max_page_sharing set to 2 (CVE-2024-0564 mitigation)");
        } catch (IOException e) {
            System.err.println("[SECURITY] Warning: could not set max_page_sharing: " + e.getMessage());
        }

        try {
            // Disable cross-NUMA merging — reduces attack surface
            setMergeAcrossNodes(false);
            System.out.println("[SECURITY] merge_across_nodes disabled (NUMA isolation)");
        } catch (IOException e) {
            System.err.println("[SECURITY] Warning: could not set merge_across_nodes: " + e.getMessage());
        }

        try {
            // Enable zero-page merging — safe (zero pages contain no secrets)
            setUseZeroPages(true);
            System.out.println("[SECURITY] use_zero_pages enabled (safe optimization)");
        } catch (IOException e) {
            System.err.println("[SECURITY] Warning: could not set use_zero_pages: " + e.getMessage());
        }
    }

    private String readString(String filename) throws IOException {
        return Files.readString(KSM_DIR.resolve(filename)).trim();
    }

    // Package-private: KsmTuner needs direct write access for periodic unmerge (run=2)
    void writeString(String filename, String content) throws IOException {
        Files.writeString(KSM_DIR.resolve(filename), content);
    }

    private int readIntSafe(String filename) {
        try {
            return Integer.parseInt(readString(filename));
        } catch (Exception e) {
            return 0;
        }
    }
}

