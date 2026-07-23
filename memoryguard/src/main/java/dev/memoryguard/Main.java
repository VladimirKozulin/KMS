package dev.memoryguard;

import dev.memoryguard.ksm.KsmController;
import dev.memoryguard.ksm.KsmTuner;
import dev.memoryguard.ksm.KsmAnomalyDetector;
import dev.memoryguard.containers.DockerClient;
import dev.memoryguard.metrics.MetricsCollector;
import dev.memoryguard.web.ApiServer;
import java.io.IOException;
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
        ksmController.applySecurityHardening();
        System.out.println("[OK] KSM security hardening parameters applied.");

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
        if (dockerClient.isDockerAvailable()) {
            System.out.println("[OK] Docker socket connection available.");
            
            // Log whitelist configuration
            String whitelist = System.getenv("MEMORYGUARD_CONTAINER_WHITELIST");
            if (whitelist == null || whitelist.trim().isEmpty()) {
                System.out.println("[SECURITY] MEMORYGUARD_CONTAINER_WHITELIST not set — " +
                        "deny-by-default: no containers will be injected with MADV_MERGEABLE.");
                System.out.println("[SECURITY] Set MEMORYGUARD_CONTAINER_WHITELIST=nginx,redis,... " +
                        "to enable injection for trusted containers only.");
            } else if ("*".equals(whitelist.trim())) {
                System.out.println("[SECURITY] MEMORYGUARD_CONTAINER_WHITELIST=* — " +
                        "ALL containers will be injected (reduced security).");
            } else {
                System.out.println("[SECURITY] MEMORYGUARD_CONTAINER_WHITELIST=" + whitelist +
                        " — only matching containers will be injected.");
            }
            
            // Запуск демона авто-инжектора MADV_MERGEABLE для контейнеров
            Thread injectorThread = new Thread(() -> {
                System.out.println("[OK] Container Madvise Injector daemon thread started.");
                // Thread-safe set — prevents data race if accessed from API thread
                java.util.Set<Integer> processedPids = ConcurrentHashMap.newKeySet();
                while (true) {
                    try {
                        if (ksmController.isRunning()) {
                            java.util.List<DockerClient.ContainerInfo> containers = dockerClient.getRunningContainers();
                            for (DockerClient.ContainerInfo container : containers) {
                                // [SECURITY] Only inject into explicitly whitelisted containers
                                if (!dockerClient.isContainerTrusted(container)) {
                                    continue;
                                }
                                java.util.List<Integer> pids = dockerClient.getContainerPids(container.id);
                                for (int pid : pids) {
                                    if (!processedPids.contains(pid)) {
                                        dockerClient.injectMadvise(pid);
                                        processedPids.add(pid);
                                    }
                                }
                            }
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
}
