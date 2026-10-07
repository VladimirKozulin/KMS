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
        if (max < 2 || max > 256) {
            throw new IllegalArgumentException("max_page_sharing must be 2-256, got: " + max);
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
     * KSM pages created via use_zero_pages (merged with the kernel zero page).
     * Not counted in pages_sharing; present on newer kernels only.
     */
    public long getKsmZeroPages() {
        return readLongSafe("ksm_zero_pages");
    }

    /**
     * Cumulative number of pages ksmd has scanned (kernel 6.7+, -1 if absent).
     * This is the direct cost of KSM: ksmd CPU time is proportional to it.
     */
    public long getPagesScanned() {
        return Files.exists(KSM_DIR.resolve("pages_scanned")) ? readLongSafe("pages_scanned") : -1;
    }

    /** Pages KSM currently saves: merged duplicates + pages merged with the zero page. */
    public long getSavedPages() {
        return getPagesSharing() + getKsmZeroPages();
    }

    /**
     * smart_scan (kernel 6.7+) skips pages that repeatedly failed to merge —
     * on GC'd heaps most pages are like that, so it cuts ksmd CPU for free.
     */
    public void enableSmartScan() {
        if (!Files.exists(KSM_DIR.resolve("smart_scan"))) return;
        try {
            writeString("smart_scan", "1");
        } catch (IOException e) {
            System.err.println("[TUNER] Could not enable smart_scan: " + e.getMessage());
        }
    }

    /**
     * Unmerging (run=2) turns every shared mapping back into a private page.
     * On a host that is overcommitted thanks to KSM — which is the whole point
     * of the tool — doing that blindly can trigger the OOM killer.
     *
     * @return true if MemAvailable stays above 10% of MemTotal after the unmerge
     */
    public boolean canAffordUnmerge() {
        long[] mem = KsmTuner.getSystemMemoryInfo();
        if (mem == null) return false;
        long cost = unmergeCostBytes();
        return mem[1] - cost > mem[0] / 10;
    }

    public long unmergeCostBytes() {
        return ((long) getPagesSharing() + getKsmZeroPages()) * 4096L;
    }

    /**
     * Applies and VERIFIES security hardening parameters on startup.
     *
     * max_page_sharing and merge_across_nodes can only be changed while no
     * pages are merged — the kernel answers EBUSY otherwise. The previous
     * implementation logged a warning and carried on, so the CVE-2024-0564
     * mitigation silently stayed at the kernel default (256). Now we unmerge
     * first (if memory headroom allows), then read the value back.
     *
     * @return true only if every required knob has its hardened value
     */
    public boolean applySecurityHardening() {
        // 2 = strongest CVE-2024-0564 mitigation, but caps dedup of identical
        // non-zero pages at 50%. MEMORYGUARD_MAX_PAGE_SHARING trades that off.
        int maxSharing = 2;
        String configured = System.getenv("MEMORYGUARD_MAX_PAGE_SHARING");
        if (configured != null && !configured.isBlank()) {
            try {
                maxSharing = Integer.parseInt(configured.trim());
            } catch (NumberFormatException e) {
                maxSharing = -1;
            }
            if (maxSharing < 2 || maxSharing > 256) {
                System.err.println("[SECURITY] MEMORYGUARD_MAX_PAGE_SHARING must be 2-256, got: " + configured);
                return false;
            }
            if (maxSharing > 2) {
                System.out.println("[SECURITY] max_page_sharing=" + maxSharing
                        + " requested — weaker CVE-2024-0564 mitigation than 2, higher savings");
            }
        }
        boolean ok = ensureKnob("max_page_sharing", String.valueOf(maxSharing), true);
        // merge_across_nodes exists only on CONFIG_NUMA kernels
        ok &= ensureKnob("merge_across_nodes", "0", false);

        try {
            // Enable zero-page merging — safe (zero pages contain no secrets)
            setUseZeroPages(true);
            System.out.println("[SECURITY] use_zero_pages enabled (safe optimization)");
        } catch (IOException e) {
            System.err.println("[SECURITY] Warning: could not set use_zero_pages: " + e.getMessage());
        }
        return ok;
    }

    private boolean ensureKnob(String knob, String value, boolean required) {
        if (!Files.exists(KSM_DIR.resolve(knob))) {
            if (required) {
                System.err.println("[SECURITY] " + knob + " not supported by this kernel — cannot harden");
            } else {
                System.out.println("[SECURITY] " + knob + " not present on this kernel — skipped");
            }
            return !required;
        }
        try {
            if (readString(knob).equals(value)) {
                System.out.println("[SECURITY] " + knob + "=" + value + " (already set)");
                return true;
            }
            try {
                writeString(knob, value);
            } catch (IOException busy) {
                // EBUSY: pages are currently merged. Unmerge, change, restore run state.
                if (!canAffordUnmerge()) {
                    System.err.println("[SECURITY] Cannot set " + knob + ": pages are merged and unmerging "
                            + (unmergeCostBytes() >> 20) + " MB would risk OOM");
                    return false;
                }
                String previousRun = readString("run");
                writeString("run", "2");
                writeString(knob, value);
                writeString("run", "2".equals(previousRun) ? "0" : previousRun);
            }
            String actual = readString(knob);
            if (!actual.equals(value)) {
                System.err.println("[SECURITY] " + knob + " is " + actual + " after write, expected " + value);
                return false;
            }
            System.out.println("[SECURITY] " + knob + "=" + value + " (verified)");
            return true;
        } catch (IOException e) {
            System.err.println("[SECURITY] Could not set " + knob + ": " + e.getMessage());
            return false;
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

    private long readLongSafe(String filename) {
        try {
            return Long.parseLong(readString(filename));
        } catch (Exception e) {
            return 0;
        }
    }
}

