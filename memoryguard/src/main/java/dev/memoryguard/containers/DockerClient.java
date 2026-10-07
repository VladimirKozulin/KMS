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
        public java.util.Map<String, String> labels = new java.util.HashMap<>();
    }

    private final TrustPolicy trustPolicy = TrustPolicy.fromEnv();

    public TrustPolicy getTrustPolicy() {
        return trustPolicy;
    }

    public boolean isDockerAvailable() {
        return Files.exists(Paths.get(SOCKET_PATH));
    }

    /**
     * Checks whether a container may join the host's KSM merge domain.
     * See {@link TrustPolicy} for the rules.
     */
    public TrustPolicy.Decision checkTrust(ContainerInfo container) {
        return trustPolicy.decide(container);
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

                JsonElement labelsEl = obj.get("Labels");
                if (labelsEl != null && labelsEl.isJsonObject()) {
                    for (var entry : labelsEl.getAsJsonObject().entrySet()) {
                        if (!entry.getValue().isJsonNull()) {
                            info.labels.put(entry.getKey(), entry.getValue().getAsString());
                        }
                    }
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

    /**
     * Marks the process's eligible anonymous memory MADV_MERGEABLE.
     * The injector re-verifies that the PID still belongs to containerId
     * after freezing it, so a recycled PID can never be injected.
     */
    public boolean injectMadvise(int pid, String containerId) {
        return runInjector(pid, containerId, false);
    }

    /** Per-process rollback: MADV_UNMERGEABLE on everything we marked. */
    public boolean unmergeProcess(int pid, String containerId) {
        return runInjector(pid, containerId, true);
    }

    private boolean runInjector(int pid, String containerId, boolean unmerge) {
        if (!isValidContainerId(containerId)) {
            System.err.println("[SECURITY] Refusing injection: invalid container ID " + containerId);
            return false;
        }
        String injectorPath = "./madvise_injector";
        if (Files.exists(Paths.get("/app/madvise_injector"))) {
            injectorPath = "/app/madvise_injector";
        } else if (Files.exists(Paths.get("target/madvise_injector"))) {
            injectorPath = "target/madvise_injector";
        }

        try {
            List<String> cmd = new ArrayList<>(List.of(injectorPath, "--container", containerId));
            if (unmerge) cmd.add("--unmerge");
            cmd.add(String.valueOf(pid));
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.redirectErrorStream(true);
            Process p = pb.start();

            // Drain output before waitFor(), otherwise a full pipe blocks the injector
            String output = new String(p.getInputStream().readAllBytes()).trim();
            int exitCode = p.waitFor();
            if (exitCode == 0) {
                System.out.println("[KSM-INJECT] " + output);
                return true;
            }
            System.err.println("[KSM-INJECT-FAIL] Exit code: " + exitCode + ". Output: " + output);
        } catch (Exception e) {
            System.err.println("[ERROR] Failed to run madvise_injector for PID " + pid + ": " + e.getMessage());
        }
        return false;
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
