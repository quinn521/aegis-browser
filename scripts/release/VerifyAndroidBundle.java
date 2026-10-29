import java.io.InputStream;
import java.security.MessageDigest;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.HexFormat;
import java.util.HashSet;
import java.util.jar.JarFile;

/** JDK 17+ source launcher. Verify every payload entry, not just the JAR footer. */
class VerifyAndroidBundle {
    public static void main(String[] args) throws Exception {
        if (args.length != 2 || !args[1].matches("[0-9a-f]{64}")) {
            throw new IllegalArgumentException("bundle.aab expected-certificate-sha256");
        }
        int count = 0;
        try (JarFile jar = new JarFile(args[0], true)) {
            var names = new HashSet<String>();
            var entries = jar.entries();
            while (entries.hasMoreElements()) {
                var entry = entries.nextElement();
                if (!names.add(entry.getName())) throw new SecurityException("Duplicate JAR entry");
                if (entry.isDirectory() || entry.getName().toUpperCase(java.util.Locale.ROOT)
                        .matches("META-INF/(MANIFEST\\.MF|[^/]+\\.(SF|RSA|DSA|EC))")) continue;
                try (InputStream stream = jar.getInputStream(entry)) {
                    byte[] buffer = new byte[1024 * 1024];
                    while (stream.read(buffer) != -1) { /* Forces JAR signature verification. */ }
                }
                Certificate[] certs = entry.getCertificates();
                if (certs == null || certs.length != 1 || !(certs[0] instanceof X509Certificate)) {
                    throw new SecurityException("Missing or ambiguous signer: " + entry.getName());
                }
                ((X509Certificate) certs[0]).checkValidity();
                String digest = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(certs[0].getEncoded()));
                if (!args[1].equals(digest)) throw new SecurityException("Unexpected upload certificate");
                count++;
            }
        }
        if (count == 0) throw new SecurityException("Empty signed bundle");
        System.out.println("Verified signed payload entries: " + count);
    }
}
