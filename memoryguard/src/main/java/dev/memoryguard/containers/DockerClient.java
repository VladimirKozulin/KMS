package dev.memoryguard.containers;

import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SocketChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

public class DockerClient {
    private static final String SOCKET_PATH = "/var/run/docker.sock";

    // Regex for valid Docker container IDs (64 hex chars)
    private static final java.util.regex.Pattern CONTAINER_ID_PATTERN =
            java.util.regex.Pattern.compile("^[a-f0-9]{12,64}$");

    public static class ContainerInfo {
        public String id;
        public String name;
        public String image;
        public long memoryUsageBytes;
    }

    public boolean isDockerAvailable() {
        return Files.exists(Paths.get(SOCKET_PATH));
    }

    // ============================================================
    // KSM Side-Channel Mitigation: Container Trust Whitelist
    // ============================================================
    // Only containers explicitly listed in MEMORYGUARD_CONTAINER_WHITELIST
    // will have MADV_MERGEABLE injected. This prevents a malicious container
    // on the same host from being merged with a victim container, closing
    // the primary KSM side-channel attack vector.
    //
    // Usage: MEMORYGUARD_CONTAINER_WHITELIST=nginx,redis,postgres
    // ============================================================

    /**
     * Checks whether a container is trusted for KSM injection.
     * Deny-by-default: if MEMORYGUARD_CONTAINER_WHITELIST is not set,
     * NO containers are injected.
     *
     * @param container the container to check
     * @return true if the container is explicitly whitelisted
     */
    public boolean isContainerTrusted(ContainerInfo container) {
        String whitelist = System.getenv("MEMORYGUARD_CONTAINER_WHITELIST");
        if (whitelist == null || whitelist.trim().isEmpty()) {
            // Deny-by-default: no whitelist = no injection
            return false;
        }

        // Special value "*" means trust all (opt-in to old behavior)
        if ("*".equals(whitelist.trim())) {
            return true;
        }

        String[] allowed = whitelist.split(",");
        for (String pattern : allowed) {
            pattern = pattern.trim();
            if (pattern.isEmpty()) continue;
            if (container.name.contains(pattern) || container.image.contains(pattern)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Validates that a container ID is a legitimate Docker hex string.
     * Prevents path traversal via malicious container IDs in cgroup paths.
     */
    public static boolean isValidContainerId(String containerId) {
        return containerId != null && CONTAINER_ID_PATTERN.matcher(containerId).matches();
    }


    public List<ContainerInfo> getRunningContainers() {
        List<ContainerInfo> list = new ArrayList<>();
        if (!isDockerAvailable()) {

            return list;
        }

        try {
            String httpResponse = sendGetRequest("/containers/json");
            if (httpResponse == null) {
    
                return list;
            }

            int bodyStart = httpResponse.indexOf("\r\n\r\n");
            if (bodyStart == -1) {
    
                return list;
            }
            String jsonBody = httpResponse.substring(bodyStart + 4);


            JsonArray array = JsonParser.parseString(jsonBody).getAsJsonArray();

            for (JsonElement el : array) {
                JsonObject obj = el.getAsJsonObject();
                ContainerInfo info = new ContainerInfo();
                info.id = obj.get("Id").getAsString();
                info.image = obj.get("Image").getAsString();
                
                JsonArray names = obj.getAsJsonArray("Names");
                if (names != null && names.size() > 0) {
                    info.name = names.get(0).getAsString().replace("/", "");
                } else {
                    info.name = info.id.substring(0, 12);
                }

                info.memoryUsageBytes = getContainerMemoryUsage(info.id);
                list.add(info);
            }
        } catch (Exception e) {
            System.err.println("[ERROR] Failed to get running containers: " + e.toString());
            e.printStackTrace();
        }

        return list;
    }

    private long getContainerMemoryUsage(String containerId) {
        // [SECURITY] Validate container ID format to prevent path traversal
        if (!isValidContainerId(containerId)) {
            System.err.println("[SECURITY] Rejected invalid container ID format: " + containerId);
            return 0;
        }

        try {
            Path cgroupV2Path = Paths.get("/sys/fs/cgroup/system.slice/docker-" + containerId + ".scope/memory.current");
            if (Files.exists(cgroupV2Path)) {
                return Long.parseLong(Files.readString(cgroupV2Path).trim());
            }

            Path altPath = Paths.get("/sys/fs/cgroup/docker/" + containerId + "/memory.current");
            if (Files.exists(altPath)) {
                return Long.parseLong(Files.readString(altPath).trim());
            }
            
            Path altPathV1 = Paths.get("/sys/fs/cgroup/memory/docker/" + containerId + "/memory.usage_in_bytes");
            if (Files.exists(altPathV1)) {
                return Long.parseLong(Files.readString(altPathV1).trim());
            }
        } catch (Exception e) {
            System.err.println("[ERROR] Failed to read memory for container " + containerId + ": " + e.toString());
        }
        return 0;
    }

    public List<Integer> getContainerPids(String containerId) {
        List<Integer> pids = new ArrayList<>();
        try {
            String httpResponse = sendGetRequest("/containers/" + containerId + "/top");
            if (httpResponse == null) return pids;

            int bodyStart = httpResponse.indexOf("\r\n\r\n");
            if (bodyStart == -1) return pids;
            String jsonBody = httpResponse.substring(bodyStart + 4);

            JsonObject obj = JsonParser.parseString(jsonBody).getAsJsonObject();
            JsonArray processes = obj.getAsJsonArray("Processes");
            JsonArray titles = obj.getAsJsonArray("Titles");
            
            int pidIndex = -1;
            for (int i = 0; i < titles.size(); i++) {
                if (titles.get(i).getAsString().equalsIgnoreCase("PID")) {
                    pidIndex = i;
                    break;
                }
            }

            if (pidIndex != -1 && processes != null) {
                for (JsonElement procEl : processes) {
                    JsonArray procArr = procEl.getAsJsonArray();
                    String pidStr = procArr.get(pidIndex).getAsString();
                    pids.add(Integer.parseInt(pidStr));
                }
            }
        } catch (Exception e) {
            System.err.println("[ERROR] Failed to get PIDs for container " + containerId + ": " + e.toString());
        }
        return pids;
    }

    public void injectMadvise(int pid) {
        String injectorPath = "./madvise_injector";
        if (Files.exists(Paths.get("/app/madvise_injector"))) {
            injectorPath = "/app/madvise_injector";
        } else if (Files.exists(Paths.get("target/madvise_injector"))) {
            injectorPath = "target/madvise_injector";
        }

        try {
            ProcessBuilder pb = new ProcessBuilder(injectorPath, String.valueOf(pid));
            pb.redirectErrorStream(true);
            Process p = pb.start();
            
            int exitCode = p.waitFor();
            String output = new String(p.getInputStream().readAllBytes()).trim();
            if (exitCode == 0) {
                System.out.println("[KSM-INJECT] " + output);
            } else {
                System.err.println("[KSM-INJECT-FAIL] Exit code: " + exitCode + ". Output: " + output);
            }
        } catch (Exception e) {
            System.err.println("[ERROR] Failed to run madvise_injector for PID " + pid + ": " + e.getMessage());
        }
    }

    private String sendGetRequest(String path) {
        try {
            UnixDomainSocketAddress address = UnixDomainSocketAddress.of(SOCKET_PATH);
            try (SocketChannel channel = SocketChannel.open(StandardProtocolFamily.UNIX)) {
                channel.connect(address);

                String request = "GET " + path + " HTTP/1.0\r\n" +
                                 "Host: localhost\r\n" +
                                 "Connection: close\r\n\r\n";

                ByteBuffer buffer = ByteBuffer.wrap(request.getBytes());
                while (buffer.hasRemaining()) {
                    channel.write(buffer);
                }

                ByteBuffer readBuffer = ByteBuffer.allocate(65536);
                StringBuilder response = new StringBuilder();
                while (channel.read(readBuffer) > 0) {
                    readBuffer.flip();
                    response.append(new String(readBuffer.array(), 0, readBuffer.limit()));
                    readBuffer.clear();
                }

                return response.toString();
            }
        } catch (IOException e) {
            return null;
        }
    }
}
