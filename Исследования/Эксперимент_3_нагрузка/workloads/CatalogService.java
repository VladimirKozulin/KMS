import com.sun.net.httpserver.HttpServer;

import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;

/**
 * Synthetic "microservice" for the KSM experiment (single-file, run with `java CatalogService.java`).
 *
 * Memory model of a typical replica:
 *  - shared catalog : built from a FIXED seed → identical content in every replica
 *                     (reference data, configs, product catalogs, ML vocabularies)
 *  - private state  : built from a per-replica random seed → never identical
 *                     (sessions, caches of user data)
 *  - request churn  : every request allocates short-lived garbage and mutates a
 *                     slice of the session cache → GC activity, CoW, volatile pages
 *
 * Env: CATALOG_MB, PRIVATE_MB, PORT
 */
public class CatalogService {
    record Product(long id, String sku, String title, String description, double price, int[] tags) {}

    static final List<Product> catalog = new ArrayList<>();
    static final Map<Integer, byte[]> sessions = new ConcurrentHashMap<>();

    public static void main(String[] args) throws Exception {
        int catalogMb = Integer.parseInt(System.getenv().getOrDefault("CATALOG_MB", "300"));
        int privateMb = Integer.parseInt(System.getenv().getOrDefault("PRIVATE_MB", "100"));
        int port = Integer.parseInt(System.getenv().getOrDefault("PORT", "8080"));

        // Shared catalog: deterministic content (~1 KB per product)
        Random shared = new Random(42);
        int products = catalogMb * 1024;
        for (int i = 0; i < products; i++) {
            catalog.add(new Product(i, "SKU-" + i, randomText(shared, 64), randomText(shared, 700),
                    shared.nextInt(100000) / 100.0, shared.ints(16, 0, 1000).toArray()));
        }

        // Private state: unique per replica
        Random priv = new Random(System.nanoTime());
        for (int i = 0; i < privateMb * 16; i++) {
            byte[] b = new byte[64 * 1024];
            priv.nextBytes(b);
            sessions.put(i, b);
        }
        System.out.printf("ready: catalog=%d products, sessions=%d%n", catalog.size(), sessions.size());

        HttpServer server = HttpServer.create(new InetSocketAddress(port), 256);
        server.setExecutor(Executors.newFixedThreadPool(8));
        server.createContext("/", exchange -> {
            Random r = new Random();
            // Read path: aggregate over a catalog window
            int start = r.nextInt(catalog.size() - 200);
            StringBuilder sb = new StringBuilder(8192);
            double sum = 0;
            for (int i = start; i < start + 200; i++) {
                Product p = catalog.get(i);
                sum += p.price();
                if (i - start < 20) sb.append(p.sku()).append(':').append(p.title(), 0, 16).append('\n');
            }
            sb.append("sum=").append(sum).append('\n');
            // Write path: mutate one private session (CoW on merged pages, churn)
            byte[] s = sessions.get(r.nextInt(sessions.size()));
            for (int i = 0; i < 64; i++) s[r.nextInt(s.length)] = (byte) r.nextInt();
            // Garbage: short-lived allocations
            Map<String, Object> tmp = new HashMap<>();
            for (int i = 0; i < 50; i++) tmp.put("k" + i, new byte[1024]);

            byte[] body = sb.toString().getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(200, body.length);
            try (OutputStream os = exchange.getResponseBody()) {
                os.write(body);
            }
        });
        server.start();
    }

    static String randomText(Random r, int len) {
        char[] c = new char[len];
        for (int i = 0; i < len; i++) c[i] = (char) ('a' + r.nextInt(26));
        return new String(c);
    }
}
