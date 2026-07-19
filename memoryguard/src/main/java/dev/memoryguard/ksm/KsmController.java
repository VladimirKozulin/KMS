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

    private String readString(String filename) throws IOException {
        return Files.readString(KSM_DIR.resolve(filename)).trim();
    }

    private void writeString(String filename, String content) throws IOException {
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
