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

    public static class ContainerInfo {
        public String id;
        public String name;
        public String image;
        public long memoryUsageBytes;
    }

    public boolean isDockerAvailable() {
        return Files.exists(Paths.get(SOCKET_PATH));
    }

    public List<ContainerInfo> getRunningContainers() {
        List<ContainerInfo> list = new ArrayList<>();
        if (!isDockerAvailable()) {
            System.out.println("[DEBUG] Docker socket is not available.");
            return list;
        }

        try {
            String httpResponse = sendGetRequest("/containers/json");
            if (httpResponse == null) {
                System.out.println("[DEBUG] sendGetRequest returned null.");
                return list;
            }

            int bodyStart = httpResponse.indexOf("\r\n\r\n");
            if (bodyStart == -1) {
                System.out.println("[DEBUG] HTTP response has no body delimiter.");
                return list;
            }
            String jsonBody = httpResponse.substring(bodyStart + 4);
            System.out.println("[DEBUG] Raw Docker JSON: " + jsonBody);

            JsonArray array = JsonParser.parseString(jsonBody).getAsJsonArray();
            System.out.println("[DEBUG] Found " + array.size() + " containers in JSON.");
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
        try {
            Path cgroupV2Path = Paths.get("/sys/fs/cgroup/system.slice/docker-" + containerId + ".scope/memory.current");
            System.out.println("[DEBUG] Checking v2 path: " + cgroupV2Path + " (exists: " + Files.exists(cgroupV2Path) + ")");
            if (Files.exists(cgroupV2Path)) {
                return Long.parseLong(Files.readString(cgroupV2Path).trim());
            }

            Path altPath = Paths.get("/sys/fs/cgroup/docker/" + containerId + "/memory.current");
            System.out.println("[DEBUG] Checking alt v2 path: " + altPath + " (exists: " + Files.exists(altPath) + ")");
            if (Files.exists(altPath)) {
                return Long.parseLong(Files.readString(altPath).trim());
            }
            
            Path altPathV1 = Paths.get("/sys/fs/cgroup/memory/docker/" + containerId + "/memory.usage_in_bytes");
            System.out.println("[DEBUG] Checking v1 path: " + altPathV1 + " (exists: " + Files.exists(altPathV1) + ")");
            if (Files.exists(altPathV1)) {
                return Long.parseLong(Files.readString(altPathV1).trim());
            }
        } catch (Exception e) {
            System.err.println("[ERROR] Failed to read memory for container " + containerId + ": " + e.toString());
        }
        return 0;
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
