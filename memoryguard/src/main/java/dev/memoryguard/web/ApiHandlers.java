package dev.memoryguard.web;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.sun.net.httpserver.HttpExchange;
import dev.memoryguard.ksm.KsmController;
import dev.memoryguard.ksm.KsmTuner;
import dev.memoryguard.metrics.MetricsCollector;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public class ApiHandlers {
    private final MetricsCollector collector;
    private final KsmController ksmController;
    private final KsmTuner ksmTuner;
    private final Gson gson = new Gson();

    public ApiHandlers(MetricsCollector collector, KsmController ksmController, KsmTuner ksmTuner) {
        this.collector = collector;
        this.ksmController = ksmController;
        this.ksmTuner = ksmTuner;
    }

    public void handleStatic(HttpExchange exchange) throws IOException {
        String path = exchange.getRequestURI().getPath();
        if (path.equals("/")) {
            path = "/index.html";
        }

        String resourcePath = "/web" + path;
        try (InputStream is = getClass().getResourceAsStream(resourcePath)) {
            if (is == null) {
                sendResponse(exchange, 404, "Not Found");
                return;
            }

            byte[] bytes = is.readAllBytes();
            String contentType = "text/plain";
            if (path.endsWith(".html")) contentType = "text/html; charset=utf-8";
            else if (path.endsWith(".css")) contentType = "text/css; charset=utf-8";
            else if (path.endsWith(".js")) contentType = "application/javascript; charset=utf-8";

            exchange.getResponseHeaders().set("Content-Type", contentType);
            exchange.sendResponseHeaders(200, bytes.length);
            try (OutputStream os = exchange.getResponseBody()) {
                os.write(bytes);
            }
        }
    }

    public void handleStatus(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equalsIgnoreCase("GET")) {
            sendResponse(exchange, 405, "Method Not Allowed");
            return;
        }

        MetricsCollector.MetricsReport report = collector.collect();
        String json = gson.toJson(report);

        exchange.getResponseHeaders().set("Content-Type", "application/json");
        byte[] bytes = json.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(200, bytes.length);
        try (OutputStream os = exchange.getResponseBody()) {
            os.write(bytes);
        }
    }

    public void handleToggle(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equalsIgnoreCase("POST")) {
            sendResponse(exchange, 405, "Method Not Allowed");
            return;
        }

        try {
            String body = new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
            JsonObject json = JsonParser.parseString(body).getAsJsonObject();
            boolean run = json.get("run").getAsBoolean();
            
            ksmController.setRunning(run);
            sendResponse(exchange, 200, "{\"success\":true}");
        } catch (Exception e) {
            sendResponse(exchange, 400, "{\"error\":\"" + e.getMessage() + "\"}");
        }
    }

    public void handleTune(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equalsIgnoreCase("POST")) {
            sendResponse(exchange, 405, "Method Not Allowed");
            return;
        }

        try {
            String body = new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
            JsonObject json = JsonParser.parseString(body).getAsJsonObject();

            if (json.has("autoTune")) {
                ksmTuner.setAutoTune(json.get("autoTune").getAsBoolean());
            }
            if (json.has("pagesToScan")) {
                ksmController.setPagesToScan(json.get("pagesToScan").getAsInt());
            }
            if (json.has("sleepMillisecs")) {
                ksmController.setSleepMillisecs(json.get("sleepMillisecs").getAsInt());
            }
            if (json.has("useZeroPages")) {
                ksmController.setUseZeroPages(json.get("useZeroPages").getAsBoolean());
            }

            sendResponse(exchange, 200, "{\"success\":true}");
        } catch (Exception e) {
            sendResponse(exchange, 400, "{\"error\":\"" + e.getMessage() + "\"}");
        }
    }

    private void sendResponse(HttpExchange exchange, int statusCode, String content) throws IOException {
        byte[] bytes = content.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(statusCode, bytes.length);
        try (OutputStream os = exchange.getResponseBody()) {
            os.write(bytes);
        }
    }
}
