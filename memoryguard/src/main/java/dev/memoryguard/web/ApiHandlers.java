package dev.memoryguard.web;

import com.google.gson.JsonObject;
import com.google.gson.JsonArray;
import com.google.gson.JsonParser;
import com.sun.net.httpserver.HttpExchange;
import dev.memoryguard.ksm.KsmController;
import dev.memoryguard.ksm.KsmTuner;
import dev.memoryguard.metrics.MetricsCollector;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

public class ApiHandlers {
    private final MetricsCollector collector;
    private final KsmController ksmController;
    private final KsmTuner ksmTuner;
    private final byte[] apiToken;

    // ksmd CPU guard rails: sleep_millisecs=0 + a huge pages_to_scan pins a core
    static final int MIN_SLEEP_MS = 10, MAX_SLEEP_MS = 10_000;
    static final int MIN_PAGES_TO_SCAN = 1, MAX_PAGES_TO_SCAN = 30_000;

    public ApiHandlers(MetricsCollector collector, KsmController ksmController, KsmTuner ksmTuner) {
        this.collector = collector;
        this.ksmController = ksmController;
        this.ksmTuner = ksmTuner;
        String token = System.getenv("MEMORYGUARD_API_TOKEN");
        this.apiToken = (token == null || token.isBlank()) ? null : token.trim().getBytes(StandardCharsets.UTF_8);
    }

    public boolean isWriteEnabled() {
        return apiToken != null;
    }

    /**
     * Mutating endpoints previously had no authentication at all: anything that
     * could reach :8787 - including a web page open in an admin's browser
     * (CSRF via a text/plain POST) - could stop KSM or retune ksmd.
     * Now: no token configured -> API is read-only; token configured -> every
     * POST needs "Authorization: Bearer <token>" and a JSON content type.
     * A custom Authorization header cannot be sent cross-origin without a CORS
     * preflight, which this server never approves.
     */
    private boolean authorizeWrite(HttpExchange exchange) throws IOException {
        if (apiToken == null) {
            sendJson(exchange, 403, error("control API disabled: set MEMORYGUARD_API_TOKEN"));
            return false;
        }
        String contentType = exchange.getRequestHeaders().getFirst("Content-Type");
        if (contentType == null || !contentType.toLowerCase().startsWith("application/json")) {
            sendJson(exchange, 415, error("Content-Type must be application/json"));
            return false;
        }
        String auth = exchange.getRequestHeaders().getFirst("Authorization");
        byte[] presented = (auth != null && auth.startsWith("Bearer "))
                ? auth.substring(7).trim().getBytes(StandardCharsets.UTF_8)
                : new byte[0];
        if (!MessageDigest.isEqual(apiToken, presented)) {
            sendJson(exchange, 401, error("invalid or missing token"));
            return false;
        }
        return true;
    }

    private static String error(String message) {
        JsonObject obj = new JsonObject();
        obj.addProperty("error", message);   // Gson escapes - no JSON injection via exception text
        return obj.toString();
    }

    private static int requireRange(JsonObject json, String field, int min, int max) {
        int value = json.get(field).getAsInt();
        if (value < min || value > max) {
            throw new IllegalArgumentException(field + " must be in [" + min + ", " + max + "]");
        }
        return value;
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

        // Ручная сборка JSON без рефлексии для совместимости с GraalVM Native
        JsonObject obj = new JsonObject();
        obj.addProperty("ksmSupported", report.ksmSupported);
        obj.addProperty("ksmRunning", report.ksmRunning);
        obj.addProperty("pagesShared", report.pagesShared);
        obj.addProperty("pagesSharing", report.pagesSharing);
        obj.addProperty("pagesUnshared", report.pagesUnshared);
        obj.addProperty("pagesVolatile", report.pagesVolatile);
        obj.addProperty("fullScans", report.fullScans);
        obj.addProperty("sleepMillisecs", report.sleepMillisecs);
        obj.addProperty("pagesToScan", report.pagesToScan);
        obj.addProperty("useZeroPages", report.useZeroPages);
        obj.addProperty("autoTune", report.autoTune);
        obj.addProperty("systemTotalMemory", report.systemTotalMemory);
        obj.addProperty("systemAvailableMemory", report.systemAvailableMemory);
        obj.addProperty("systemMemoryUsagePercent", report.systemMemoryUsagePercent);
        obj.addProperty("savedMemoryMb", report.savedMemoryMb);
        obj.addProperty("monthlySavingsUsd", report.monthlySavingsUsd);
        obj.addProperty("maxPageSharing", ksmController.getMaxPageSharing());
        obj.addProperty("anomalyCooldown", ksmTuner.isInAnomalyCooldown());
        obj.addProperty("lastSecurityEvent", ksmTuner.getLastSecurityEvent());
        obj.addProperty("controlApiEnabled", isWriteEnabled());

        JsonArray arr = new JsonArray();
        if (report.containers != null) {
            for (dev.memoryguard.containers.DockerClient.ContainerInfo c : report.containers) {
                JsonObject cObj = new JsonObject();
                cObj.addProperty("id", c.id);
                cObj.addProperty("name", c.name);
                cObj.addProperty("image", c.image);
                cObj.addProperty("memoryUsageBytes", c.memoryUsageBytes);
                arr.add(cObj);
            }
        }
        obj.add("containers", arr);

        String json = obj.toString();

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
        if (!authorizeWrite(exchange)) return;

        try {
            String body = new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
            JsonObject json = JsonParser.parseString(body).getAsJsonObject();
            boolean run = json.get("run").getAsBoolean();

            ksmController.setRunning(run);
            sendJson(exchange, 200, "{\"success\":true}");
        } catch (Exception e) {
            sendJson(exchange, 400, error(String.valueOf(e.getMessage())));
        }
    }

    public void handleTune(HttpExchange exchange) throws IOException {
        if (!exchange.getRequestMethod().equalsIgnoreCase("POST")) {
            sendResponse(exchange, 405, "Method Not Allowed");
            return;
        }
        if (!authorizeWrite(exchange)) return;

        try {
            String body = new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
            JsonObject json = JsonParser.parseString(body).getAsJsonObject();

            // Validate everything before applying anything
            Integer pagesToScan = json.has("pagesToScan")
                    ? requireRange(json, "pagesToScan", MIN_PAGES_TO_SCAN, MAX_PAGES_TO_SCAN) : null;
            Integer sleepMs = json.has("sleepMillisecs")
                    ? requireRange(json, "sleepMillisecs", MIN_SLEEP_MS, MAX_SLEEP_MS) : null;

            if (json.has("autoTune")) {
                ksmTuner.setAutoTune(json.get("autoTune").getAsBoolean());
            }
            if (pagesToScan != null) {
                ksmController.setPagesToScan(pagesToScan);
            }
            if (sleepMs != null) {
                ksmController.setSleepMillisecs(sleepMs);
            }
            if (json.has("useZeroPages")) {
                ksmController.setUseZeroPages(json.get("useZeroPages").getAsBoolean());
            }

            sendJson(exchange, 200, "{\"success\":true}");
        } catch (Exception e) {
            sendJson(exchange, 400, error(String.valueOf(e.getMessage())));
        }
    }

    private void sendJson(HttpExchange exchange, int statusCode, String json) throws IOException {
        exchange.getResponseHeaders().set("Content-Type", "application/json");
        sendResponse(exchange, statusCode, json);
    }

    private void sendResponse(HttpExchange exchange, int statusCode, String content) throws IOException {
        byte[] bytes = content.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(statusCode, bytes.length);
        try (OutputStream os = exchange.getResponseBody()) {
            os.write(bytes);
        }
    }
}
