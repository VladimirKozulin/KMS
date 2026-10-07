package dev.memoryguard.containers;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Decides which containers may have their memory marked MADV_MERGEABLE.
 *
 * KSM keeps ONE global merge tree per NUMA node: every mergeable page on the
 * host is compared with every other one. So "trusted" here means "may share
 * physical pages with every other trusted container" — the set of trusted
 * containers is a single security domain.
 *
 * Rules, evaluated in order (first match wins):
 *   1. label memoryguard.ksm=false                → deny (opt-out always wins)
 *   2. label memoryguard.domain != host domain   → deny (different trust domain)
 *   3. image is a data store (redis, memcached…) → deny unless explicitly opted in
 *      — these keep attacker-controlled request data in memory, which is exactly
 *      what remote dedup attacks exploit (Schwarzl et al., NDSS 2022)
 *   4. label memoryguard.ksm=true                 → allow, ONLY if MEMORYGUARD_TRUST_LABELS=true
 *   5. name/image matches MEMORYGUARD_CONTAINER_WHITELIST (exact or glob) → allow
 *   6. otherwise                                  → deny
 *
 * Labels are set by whoever starts the container. On a multi-tenant host that
 * is the tenant — i.e. potentially the attacker — so by default labels can
 * only RESTRICT merging, never grant it. Set MEMORYGUARD_TRUST_LABELS=true only
 * when labels are controlled by the platform (admission controller, CI).
 */
public final class TrustPolicy {
    public static final String LABEL_KSM = "memoryguard.ksm";
    public static final String LABEL_DOMAIN = "memoryguard.domain";
    public static final String DEFAULT_DOMAIN = "default";

    static final List<String> DEFAULT_DENY_IMAGES = List.of(
            "redis", "valkey", "keydb", "memcached", "dragonfly",
            "mysql", "mariadb", "postgres", "mongo", "cassandra", "elasticsearch",
            "vault", "keycloak", "etcd", "consul");

    public record Decision(boolean trusted, String reason) {}

    private final List<String> whitelistGlobs;
    private final List<Pattern> whitelist;
    private final boolean trustAll;
    private final String hostDomain;
    private final List<String> denyImages;
    private final boolean trustLabels;

    TrustPolicy(String whitelistSpec, String hostDomain, String denyImagesSpec, boolean trustLabels) {
        String spec = whitelistSpec == null ? "" : whitelistSpec.trim();
        this.trustAll = "*".equals(spec);
        this.whitelistGlobs = trustAll ? List.of() : splitList(spec);
        this.whitelist = compileGlobs(whitelistGlobs);
        this.hostDomain = (hostDomain == null || hostDomain.isBlank()) ? DEFAULT_DOMAIN : hostDomain.trim();
        this.denyImages = denyImagesSpec == null
                ? DEFAULT_DENY_IMAGES
                : splitList(denyImagesSpec);
        this.trustLabels = trustLabels;
    }

    public static TrustPolicy fromEnv() {
        return new TrustPolicy(
                System.getenv("MEMORYGUARD_CONTAINER_WHITELIST"),
                System.getenv("MEMORYGUARD_TRUST_DOMAIN"),
                System.getenv("MEMORYGUARD_DENY_IMAGES"),
                "true".equalsIgnoreCase(System.getenv("MEMORYGUARD_TRUST_LABELS")));
    }

    public String describe() {
        return String.format("domain=%s, whitelist=%s, trustLabels=%s, denyImages=%s",
                hostDomain, trustAll ? "*" : whitelistGlobs, trustLabels, denyImages);
    }

    public Decision decide(DockerClient.ContainerInfo c) {
        Map<String, String> labels = c.labels == null ? Collections.emptyMap() : c.labels;
        String ksmLabel = labels.get(LABEL_KSM);

        if ("false".equalsIgnoreCase(ksmLabel)) {
            return new Decision(false, "opted out via label " + LABEL_KSM + "=false");
        }

        String domain = labels.getOrDefault(LABEL_DOMAIN, DEFAULT_DOMAIN);
        if (!domain.equals(hostDomain)) {
            return new Decision(false, "trust domain '" + domain + "' != host domain '" + hostDomain + "'");
        }

        boolean labelOptIn = trustLabels && "true".equalsIgnoreCase(ksmLabel);
        String repo = imageRepository(c.image);
        if (!labelOptIn && denyImages.contains(repo)) {
            return new Decision(false, "image '" + repo + "' holds attacker-controlled data "
                    + "(remote dedup risk); override with label " + LABEL_KSM + "=true + MEMORYGUARD_TRUST_LABELS=true");
        }

        if (labelOptIn) {
            return new Decision(true, "label " + LABEL_KSM + "=true");
        }
        if (trustAll) {
            return new Decision(true, "whitelist=*");
        }
        for (int i = 0; i < whitelist.size(); i++) {
            Pattern p = whitelist.get(i);
            String glob = whitelistGlobs.get(i);
            if (p.matcher(c.name).matches()) return new Decision(true, "whitelist name " + glob);
            if (p.matcher(c.image).matches()) return new Decision(true, "whitelist image " + glob);
        }
        return new Decision(false, "not whitelisted");
    }

    /** "docker.io/library/redis:7.2-alpine@sha256:..." → "redis" */
    static String imageRepository(String image) {
        if (image == null) return "";
        String s = image;
        int at = s.indexOf('@');
        if (at >= 0) s = s.substring(0, at);
        int slash = s.lastIndexOf('/');
        if (slash >= 0) s = s.substring(slash + 1);
        int colon = s.indexOf(':');
        if (colon >= 0) s = s.substring(0, colon);
        return s;
    }

    /**
     * Exact match by default; '*' is the only wildcard. The previous
     * substring matching let "evil-redis-proxy" pass a "redis" whitelist entry.
     */
    private static List<Pattern> compileGlobs(List<String> globs) {
        List<Pattern> patterns = new ArrayList<>();
        for (String entry : globs) {
            patterns.add(Pattern.compile(globToRegex(entry)));
        }
        return patterns;
    }

    static String globToRegex(String glob) {
        StringBuilder regex = new StringBuilder();
        String[] parts = glob.split("\\*", -1);
        for (int i = 0; i < parts.length; i++) {
            if (i > 0) regex.append(".*");
            regex.append(Pattern.quote(parts[i]));
        }
        return regex.toString();
    }

    private static List<String> splitList(String spec) {
        List<String> out = new ArrayList<>();
        for (String s : spec.split(",")) {
            s = s.trim();
            if (!s.isEmpty()) out.add(s);
        }
        return out;
    }
}
