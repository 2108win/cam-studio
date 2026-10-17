// Cross-checks docs/protocol-vectors.json (pure Python) against the JDK's
// own ECDH and AES-GCM. Run: java tools/VerifyVectors.java
import java.math.BigInteger;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.AlgorithmParameters;
import java.security.KeyFactory;
import java.security.interfaces.ECPublicKey;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPoint;
import java.security.spec.ECPrivateKeySpec;
import java.security.spec.ECPublicKeySpec;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public class VerifyVectors {
    static String field(String json, String name) {
        Matcher m = Pattern.compile("\"" + name + "\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
        if (!m.find()) throw new IllegalStateException("missing " + name);
        return m.group(1);
    }

    public static void main(String[] args) throws Exception {
        String json = Files.readString(Path.of("docs/protocol-vectors.json"));
        HexFormat hex = HexFormat.of();

        AlgorithmParameters params = AlgorithmParameters.getInstance("EC");
        params.init(new ECGenParameterSpec("secp256r1"));
        ECParameterSpec spec = params.getParameterSpec(ECParameterSpec.class);
        KeyFactory kf = KeyFactory.getInstance("EC");

        var pcPriv = kf.generatePrivate(new ECPrivateKeySpec(new BigInteger(field(json, "pcPrivate"), 16), spec));
        byte[] phonePub = hex.parseHex(field(json, "phonePublic"));
        ECPoint point = new ECPoint(new BigInteger(1, Arrays.copyOfRange(phonePub, 1, 33)),
                                    new BigInteger(1, Arrays.copyOfRange(phonePub, 33, 65)));
        ECPublicKey phoneKey = (ECPublicKey) kf.generatePublic(new ECPublicKeySpec(point, spec));

        KeyAgreement ka = KeyAgreement.getInstance("ECDH");
        ka.init(pcPriv);
        ka.doPhase(phoneKey, true);
        String z = hex.formatHex(ka.generateSecret());
        check("Z (JDK ECDH)", z, field(json, "Z"));

        byte[] key = hex.parseHex(field(json, "K_up"));
        byte[] plaintext = hex.parseHex(field(json, "sealedPlaintext"));
        byte[] nonce = hex.parseHex("00000001" + "0000000000000000");
        byte[] aad = hex.parseHex("02" + "0000000000000000");
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, nonce));
        c.updateAAD(aad);
        byte[] sealed = c.doFinal(plaintext);
        String record = String.format("%08x", 1 + 8 + sealed.length) + "02" + "0000000000000000" + hex.formatHex(sealed);
        check("sealed record (JDK AES-GCM)", record, field(json, "sealedRecord"));
    }

    static void check(String what, String actual, String expected) {
        if (!actual.equals(expected)) {
            System.out.println("MISMATCH " + what + "\n  jdk:    " + actual + "\n  python: " + expected);
            System.exit(1);
        }
        System.out.println("OK  " + what);
    }
}
