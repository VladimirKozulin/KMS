package dev.memoryguard;

import dev.memoryguard.ksm.KsmController;
import dev.memoryguard.ksm.KsmTuner;
import dev.memoryguard.containers.DockerClient;
import dev.memoryguard.metrics.MetricsCollector;
import dev.memoryguard.web.ApiServer;
import java.io.IOException;

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

        KsmTuner tuner = new KsmTuner(ksmController);
        Thread tunerThread = new Thread(tuner, "KsmTunerThread");
        tunerThread.setDaemon(true);
        tunerThread.start();
        System.out.println("[OK] Adaptive KsmTuner daemon thread started.");

        DockerClient dockerClient = new DockerClient();
        if (dockerClient.isDockerAvailable()) {
            System.out.println("[OK] Docker socket connection available.");
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

        try {
            Thread.currentThread().join();
        } catch (InterruptedException e) {
            System.out.println("Main thread interrupted.");
        }
    }
}
