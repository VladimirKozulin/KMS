package dev.memoryguard;

import dev.memoryguard.ksm.KsmController;
import dev.memoryguard.ksm.KsmTuner;
import dev.memoryguard.ksm.KsmAnomalyDetector;
import dev.memoryguard.containers.DockerClient;
import dev.memoryguard.containers.TrustPolicy;
import dev.memoryguard.metrics.MetricsCollector;
import dev.memoryguard.web.ApiServer;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.concurrent.ConcurrentHashMap;

public class Main {
    public static void main(String[] args) {
        System.out.println("=== Starting MemoryGuard Agent ===");
        
        KsmController ksmController = new KsmController();
        if (!ksmController.isKsmSupported()) {
            System.err.println("[ERROR] Kernel Samepage Merging (KSM) is not supported or not enabled in the OS kernel.");
            System.err.println("[INFO] Make sure you are running on a Linux system with KSM support enabled.");
            System.exit(1);
        }
        System.out.println("[OK] KSM kernel subsystem detected.");

        // ============================================================
        // KSM Side-Channel Mitigation: Apply Security Hardening
        // ============================================================
        // Sets max_page_sharing=2 (CVE-2024-0564), merge_across_nodes=0,
        // and use_zero_pages=1.
        boolean hardened = ksmController.applySecurityHardening();
        // Fail closed: without verified hardening we don't mark anything mergeable.
        // MEMORYGUARD_STRICT=false restores the old "warn and continue" behaviour.
        boolean strict = !"false".equalsIgnoreCase(System.getenv("MEMORYGUARD_STRICT"));
        boolean injectionAllowed = hardened || !strict;
        if (hardened) {
            System.out.println("[OK] KSM security hardening parameters applied and verified.");
        } else if (strict) {
            System.err.println("[SECURITY] Hardening could not be verified — container injection DISABLED "
                    + "(set MEMORYGUARD_STRICT=false to override).");
        } else {
            System.err.println("[SECURITY] Hardening could not be verified — continuing (MEMORYGUARD_STRICT=false).");
        }

        // Create anomaly detector for side-channel attack monitoring
        KsmAnomalyDetector anomalyDetector = new KsmAnomalyDetector(ksmController);
        System.out.println("[OK] KSM anomaly detector initialized.");

        // Create tuner with anomaly detector integration
        KsmTuner tuner = new KsmTuner(ksmController, anomalyDetector);
        Thread tunerThread = new Thread(tuner, "KsmTunerThread");
        tunerThread.setDaemon(true);
        tunerThread.start();
        System.out.println("[OK] Adaptive KsmTuner daemon thread started (with anti-timing + periodic unmerge).");

        DockerClient dockerClient = new DockerClient();
        if (dockerClient.isDockerAvailable() && injectionAllowed) {
            System.out.println("[OK] Docker socket connection available.");
            System.out.println("[SECURITY] Trust policy: " + dockerClient.getTrustPolicy().describe());

            // Запуск демона авто-инжектора MADV_MERGEABLE для контейнеров
            Thread injectorThread = new Thread(() -> {
                System.out.println("[OK] Container Madvise Injector daemon thread started.");
                // Key = "pid:starttime" — a recycled PID is a different process
                java.util.Set<String> processed = ConcurrentHashMap.newKeySet();
                java.util.Map<String, Boolean> loggedDecisions = new ConcurrentHashMap<>();
                while (true) {
                    try {
                        if (ksmController.isRunning()) {
                            java.util.List<DockerClient.ContainerInfo> containers = dockerClient.getRunningContainers();
                            java.util.Set<String> alive = new java.util.HashSet<>();
                            for (DockerClient.ContainerInfo container : containers) {
                                TrustPolicy.Decision decision = dockerClient.checkTrust(container);
                                if (loggedDecisions.put(container.id, decision.trusted()) == null) {
                                    System.out.println("[SECURITY] " + container.name + ": "
                                            + (decision.trusted() ? "MERGE" : "skip") + " — " + decision.reason());
                                }
                                if (!decision.trusted()) {
                                    continue;
                                }
                                for (int pid : dockerClient.getContainerPids(container.id)) {
                                    String key = pid + ":" + processStartTime(pid);
                                    alive.add(key);
                                    // Data stores are refused by process name too: the image
                                    // name is easy to lose (retag → bare sha256 ID) or disguise
                                    String comm = processComm(pid);
                                    if (!decision.explicitOptIn() && TrustPolicy.isDataStoreProcess(comm)) {
                                        if (processed.add(key)) {
                                            System.out.println("[SECURITY] " + container.name + ": skip PID " + pid
                                                    + " (" + comm + ") — data store process, remote dedup risk");
                                        }
                                        continue;
                                    }
                                    if (!processed.contains(key)
                                            && dockerClient.injectMadvise(pid, container.id)) {
                                        processed.add(key);
                                    }
                                }
                            }
                            processed.retainAll(alive);   // forget exited processes
                            loggedDecisions.keySet().retainAll(
                                    containers.stream().map(c -> c.id).toList());
                        }
                        Thread.sleep(30000); // Сканирование контейнеров каждые 30 секунд
                    } catch (InterruptedException e) {
                        break;
                    } catch (Exception e) {
                        System.err.println("[ERROR] Exception in Container Madvise Injector daemon: " + e.getMessage());
                    }
                }
            }, "ContainerMadviseInjectorThread");
            injectorThread.setDaemon(true);
            injectorThread.start();
        } else if (!injectionAllowed) {
            System.out.println("[SECURITY] Container injection disabled (hardening not verified).");
        } else {
            System.out.println("[WARNING] Docker socket /var/run/docker.sock not found. Container discovery disabled.");
        }

        MetricsCollector collector = new MetricsCollector(ksmController, tuner, dockerClient);
        
        int port = 8787;
        ApiServer server = new ApiServer(port, collector, ksmController, tuner);
        try {
            server.start();
            System.out.println("[OK] MemoryGuard Dashboard API Server is running on http://localhost:" + port);
        } catch (IOException e) {
            System.err.println("[ERROR] Failed to start HTTP server: " + e.getMessage());
            System.exit(1);
        }

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            System.out.println("Shutting down MemoryGuard Agent...");
            server.stop();
            tuner.stop();
        }));

        System.out.println("=== MemoryGuard Agent fully started with KSM side-channel protection ===");

        try {
            Thread.currentThread().join();
        } catch (InterruptedException e) {
            System.out.println("Main thread interrupted.");
        }
    }

    static String processComm(int pid) {
        try {
            return Files.readString(Paths.get("/proc/" + pid + "/comm")).trim();
        } catch (Exception e) {
            return null;
        }
    }

    /** Field 22 of /proc/PID/stat (start time in clock ticks), or "?" if unreadable. */
    static String processStartTime(int pid) {
        try {
            String stat = Files.readString(Paths.get("/proc/" + pid + "/stat"));
            String[] fields = stat.substring(stat.lastIndexOf(')') + 2).split(" ");
            return fields[19];
        } catch (Exception e) {
            return "?";
        }
    }
}
