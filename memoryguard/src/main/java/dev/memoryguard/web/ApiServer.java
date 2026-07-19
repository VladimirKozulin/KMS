package dev.memoryguard.web;

import com.sun.net.httpserver.HttpServer;
import dev.memoryguard.ksm.KsmController;
import dev.memoryguard.ksm.KsmTuner;
import dev.memoryguard.metrics.MetricsCollector;
import java.io.IOException;
import java.net.InetSocketAddress;

public class ApiServer {
    private final int port;
    private final MetricsCollector collector;
    private final KsmController ksmController;
    private final KsmTuner ksmTuner;
    private HttpServer server;

    public ApiServer(int port, MetricsCollector collector, KsmController ksmController, KsmTuner ksmTuner) {
        this.port = port;
        this.collector = collector;
        this.ksmController = ksmController;
        this.ksmTuner = ksmTuner;
    }

    public void start() throws IOException {
        server = HttpServer.create(new InetSocketAddress(port), 0);
        ApiHandlers handlers = new ApiHandlers(collector, ksmController, ksmTuner);

        server.createContext("/", handlers::handleStatic);
        server.createContext("/api/status", handlers::handleStatus);
        server.createContext("/api/ksm/toggle", handlers::handleToggle);
        server.createContext("/api/ksm/tune", handlers::handleTune);

        server.setExecutor(null); // default executor
        server.start();
        System.out.println("HTTP Server started on port " + port);
    }

    public void stop() {
        if (server != null) {
            server.stop(0);
        }
    }
}
