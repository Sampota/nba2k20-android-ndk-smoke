package com.sampota.patchapktool;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import androidx.documentfile.provider.DocumentFile;

import com.android.apksig.ApkSigner;
import com.android.apksig.ApkVerifier;
import org.bouncycastle.asn1.x500.X500Name;
import org.bouncycastle.cert.X509CertificateHolder;
import org.bouncycastle.cert.jcajce.JcaX509CertificateConverter;
import org.bouncycastle.cert.jcajce.JcaX509v3CertificateBuilder;
import org.bouncycastle.jce.provider.BouncyCastleProvider;
import org.bouncycastle.operator.ContentSigner;
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.SecureRandom;
import java.security.Security;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

public class MainActivity extends Activity {
    private static final int PICK_APK = 10;
    private static final int PICK_SO = 11;
    private static final int PICK_OUTPUT = 12;
    private static final String PREFS = "patcher";
    private static final int ALIGNMENT = 4096;

    private Uri apkUri;
    private Uri soUri;
    private Uri outputTreeUri;
    private String apkName = "";
    private String soName = "";
    private String outputName = "No output folder selected";
    private TextView apkValue;
    private TextView soValue;
    private TextView outputValue;
    private TextView statusValue;
    private Spinner abiSpinner;
    private EditText destinationName;
    private Button patchButton;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(0xFF101827);
        getWindow().setNavigationBarColor(0xFF101827);
        buildUi();
        restoreSelection();
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(0xFFF3F5F9);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(18), dp(22), dp(18), dp(28));
        scroll.addView(page);

        TextView title = text("Patch APK Tool", 28, 0xFF101827, true);
        page.addView(title);
        TextView subtitle = text("Import a replacement native library into an APK, then sign it.", 14, 0xFF586579, false);
        LinearLayout.LayoutParams subParams = params(-1, -2);
        subParams.topMargin = dp(5);
        page.addView(subtitle, subParams);

        LinearLayout apkCard = card(page, "1  •  Select base APK");
        apkValue = text("No APK selected", 13, 0xFF697586, false);
        apkCard.addView(apkValue, spaced());
        addButton(apkCard, "Browse APK", v -> pickFile(PICK_APK));

        LinearLayout soCard = card(page, "2  •  Select replacement LIB");
        soValue = text("No .so library selected", 13, 0xFF697586, false);
        soCard.addView(soValue, spaced());
        addButton(soCard, "Browse .so Library", v -> pickFile(PICK_SO));

        LinearLayout options = card(page, "3  •  Patch destination");
        TextView abiLabel = text("Target ABI", 13, 0xFF334155, true);
        options.addView(abiLabel, spaced());
        abiSpinner = new Spinner(this);
        String[] abis = {"arm64-v8a", "armeabi-v7a", "x86_64", "x86"};
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, abis);
        abiSpinner.setAdapter(adapter);
        options.addView(abiSpinner, spaced());

        TextView filenameLabel = text("Library filename inside APK", 13, 0xFF334155, true);
        options.addView(filenameLabel, spaced());
        destinationName = new EditText(this);
        destinationName.setSingleLine(true);
        destinationName.setText("libshotmeter.so");
        destinationName.setTextSize(15);
        destinationName.setHint("libexample.so");
        destinationName.setPadding(dp(12), dp(8), dp(12), dp(8));
        options.addView(destinationName, spaced());
        TextView hint = text("The selected .so is renamed to this entry. The default is libshotmeter.so.", 12, 0xFF697586, false);
        options.addView(hint, spaced());

        LinearLayout folderCard = card(page, "4  •  Output folder");
        outputValue = text(outputName, 13, 0xFF697586, false);
        folderCard.addView(outputValue, spaced());
        addButton(folderCard, "Choose Output Folder", v -> pickOutputFolder());

        patchButton = new Button(this);
        patchButton.setText("PATCH & SIGN APK");
        patchButton.setTextSize(15);
        patchButton.setAllCaps(false);
        patchButton.setTextColor(0xFFFFFFFF);
        patchButton.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFF185ADB));
        LinearLayout.LayoutParams patchParams = params(-1, dp(54));
        patchParams.topMargin = dp(18);
        page.addView(patchButton, patchParams);
        patchButton.setOnClickListener(v -> startPatch());

        LinearLayout statusCard = card(page, "Status");
        statusValue = text("Ready. Select an APK, a .so file, and an output folder.", 13, 0xFF334155, false);
        statusValue.setMinLines(2);
        statusCard.addView(statusValue, spaced());

        TextView footer = text("The original APK is never overwritten. Existing output files are kept; new files receive a numbered name.", 12, 0xFF697586, false);
        LinearLayout.LayoutParams footParams = params(-1, -2);
        footParams.topMargin = dp(14);
        page.addView(footer, footParams);

        setContentView(scroll);
    }

    private LinearLayout card(LinearLayout parent, String heading) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(15), dp(14), dp(15), dp(15));
        box.setBackgroundColor(0xFFFFFFFF);
        LinearLayout.LayoutParams p = params(-1, -2);
        p.topMargin = dp(15);
        parent.addView(box, p);
        TextView h = text(heading, 16, 0xFF101827, true);
        box.addView(h);
        return box;
    }

    private void addButton(LinearLayout parent, String caption, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(caption);
        b.setTextSize(14);
        b.setAllCaps(false);
        b.setOnClickListener(listener);
        LinearLayout.LayoutParams p = params(-1, dp(48));
        p.topMargin = dp(10);
        parent.addView(b, p);
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(size);
        t.setTextColor(color);
        if (bold) t.setTypeface(null, android.graphics.Typeface.BOLD);
        return t;
    }

    private LinearLayout.LayoutParams spaced() {
        LinearLayout.LayoutParams p = params(-1, -2);
        p.topMargin = dp(8);
        return p;
    }

    private LinearLayout.LayoutParams params(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    private int dp(float v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private void pickFile(int request) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, request);
    }

    private void pickOutputFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, PICK_OUTPUT);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try {
            int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (Exception ignored) { }

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (requestCode == PICK_APK) {
            String name = queryName(uri);
            if (name == null || !name.toLowerCase(Locale.ROOT).endsWith(".apk")) {
                toast("Choose an .apk file.");
                return;
            }
            apkUri = uri;
            apkName = name;
            prefs.edit().putString("apk", uri.toString()).putString("apkName", name).apply();
            apkValue.setText(name);
        } else if (requestCode == PICK_SO) {
            String name = queryName(uri);
            if (name == null || !name.toLowerCase(Locale.ROOT).endsWith(".so")) {
                toast("Choose a .so shared library.");
                return;
            }
            soUri = uri;
            soName = name;
            prefs.edit().putString("so", uri.toString()).putString("soName", name).apply();
            soValue.setText(name);
        } else if (requestCode == PICK_OUTPUT) {
            outputTreeUri = uri;
            DocumentFile folder = DocumentFile.fromTreeUri(this, uri);
            outputName = folder != null && folder.getName() != null ? folder.getName() : "Selected folder";
            prefs.edit().putString("output", uri.toString()).putString("outputName", outputName).apply();
            outputValue.setText(outputName);
        }
    }

    private void restoreSelection() {
        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        String a = p.getString("apk", null);
        String s = p.getString("so", null);
        String o = p.getString("output", null);
        if (a != null) apkUri = Uri.parse(a);
        if (s != null) soUri = Uri.parse(s);
        if (o != null) outputTreeUri = Uri.parse(o);
        apkName = p.getString("apkName", "");
        soName = p.getString("soName", "");
        outputName = p.getString("outputName", "No output folder selected");
        apkValue.setText(apkName.isEmpty() ? "No APK selected" : apkName);
        soValue.setText(soName.isEmpty() ? "No .so library selected" : soName);
        outputValue.setText(outputName);
    }

    private String queryName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (i >= 0) return c.getString(i);
            }
        } catch (Exception ignored) { }
        return uri.getLastPathSegment();
    }

    private void startPatch() {
        if (apkUri == null || soUri == null || outputTreeUri == null) {
            toast("Select the base APK, replacement .so, and output folder first.");
            return;
        }
        String targetName = destinationName.getText().toString().trim();
        if (!targetName.matches("lib[A-Za-z0-9_.-]+\\.so")) {
            toast("Library filename must look like libname.so.");
            return;
        }
        String abi = (String) abiSpinner.getSelectedItem();
        Uri selectedApk = apkUri;
        Uri selectedSo = soUri;
        Uri selectedOutput = outputTreeUri;
        String selectedName = apkName;
        patchButton.setEnabled(false);
        setStatus("Preparing files…");

        executor.execute(() -> {
            File input = new File(getCacheDir(), "selected-base.apk");
            File replacement = new File(getCacheDir(), "selected-library.so");
            File unsigned = new File(getCacheDir(), "patched-unsigned.apk");
            File signed = new File(getCacheDir(), "patched-signed.apk");
            try {
                copyUriToFile(selectedApk, input);
                copyUriToFile(selectedSo, replacement);
                byte[] libBytes = readAll(replacement);
                validateElf(libBytes, abi);
                patchZip(input, libBytes, unsigned, abi, targetName);
                signAndVerify(unsigned, signed);
                String outputFileName = makeOutputName(selectedName);
                DocumentFile folder = DocumentFile.fromTreeUri(this, selectedOutput);
                if (folder == null || !folder.isDirectory() || !folder.canWrite()) {
                    throw new IOException("The selected output folder is no longer writable. Choose it again.");
                }
                String unique = uniqueName(folder, outputFileName);
                DocumentFile destination = folder.createFile("application/vnd.android.package-archive", unique);
                if (destination == null) throw new IOException("Could not create the output APK in that folder.");
                try (InputStream in = new FileInputStream(signed);
                     OutputStream out = getContentResolver().openOutputStream(destination.getUri(), "w")) {
                    if (out == null) throw new IOException("Could not open the output file for writing.");
                    copy(in, out);
                    out.flush();
                } catch (Exception e) {
                    destination.delete();
                    throw e;
                }
                runOnUiThread(() -> {
                    patchButton.setEnabled(true);
                    setStatus("SUCCESS\n" + unique + "\nLibrary: lib/" + abi + "/" + targetName + "\nSigned with V1 + V2 + V3.");
                    toast("Patched APK saved.");
                });
            } catch (Exception e) {
                String message = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                runOnUiThread(() -> {
                    patchButton.setEnabled(true);
                    setStatus("FAILED\n" + message);
                    toast("Patching failed: " + message);
                });
            } finally {
                input.delete();
                replacement.delete();
                unsigned.delete();
                signed.delete();
            }
        });
    }

    private String makeOutputName(String original) {
        String base = original == null ? "patched" : original;
        if (base.toLowerCase(Locale.ROOT).endsWith(".apk")) base = base.substring(0, base.length() - 4);
        base = base.replaceAll("[^A-Za-z0-9._-]", "_");
        if (base.isEmpty()) base = "patched";
        return base + "_PATCHED.apk";
    }

    private String uniqueName(DocumentFile folder, String proposed) {
        if (folder.findFile(proposed) == null) return proposed;
        String stem = proposed.substring(0, proposed.length() - 4);
        for (int i = 2; i < 10000; i++) {
            String candidate = stem + "_" + i + ".apk";
            if (folder.findFile(candidate) == null) return candidate;
        }
        return stem + "_" + System.currentTimeMillis() + ".apk";
    }

    private void setStatus(String s) {
        statusValue.setText(s);
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    private void copyUriToFile(Uri uri, File destination) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(destination)) {
            if (in == null) throw new IOException("Unable to read selected file: " + uri);
            copy(in, out);
        }
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        int n;
        while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
    }

    private static byte[] readAll(File f) throws IOException {
        try (InputStream in = new FileInputStream(f); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            copy(in, out);
            return out.toByteArray();
        }
    }

    private void validateElf(byte[] b, String abi) throws IOException {
        if (b.length < 64 || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') {
            throw new IOException("Selected .so is not a valid ELF shared library.");
        }
        int elfClass = b[4] & 0xff;
        int endian = b[5] & 0xff;
        if (endian != 1) throw new IOException("Only little-endian Android ELF libraries are supported.");
        int machine = (b[18] & 0xff) | ((b[19] & 0xff) << 8);
        int expectedClass;
        int expectedMachine;
        switch (abi) {
            case "arm64-v8a": expectedClass = 2; expectedMachine = 183; break;
            case "armeabi-v7a": expectedClass = 1; expectedMachine = 40; break;
            case "x86_64": expectedClass = 2; expectedMachine = 62; break;
            case "x86": expectedClass = 1; expectedMachine = 3; break;
            default: throw new IOException("Unsupported ABI: " + abi);
        }
        int elfType = (b[16] & 0xff) | ((b[17] & 0xff) << 8);
        if (elfClass != expectedClass || machine != expectedMachine || elfType != 3) {
            throw new IOException("Library architecture does not match " + abi + " or is not an ELF shared object.");
        }
    }

    private void patchZip(File input, byte[] library, File output, String abi, String soname) throws Exception {
        String targetPath = "lib/" + abi + "/" + soname;
        try (ZipFile zip = new ZipFile(input)) {
            int found = 0;
            Enumeration<? extends ZipEntry> scan = zip.entries();
            while (scan.hasMoreElements()) {
                if (scan.nextElement().getName().equals(targetPath)) found++;
            }
            if (found > 1) throw new IOException("Base APK contains duplicate target library entries.");
            try (FileOutputStream fos = new FileOutputStream(output);
                 CountingOutputStream counting = new CountingOutputStream(fos);
                 ZipOutputStream zout = new ZipOutputStream(counting)) {
                zout.setLevel(Deflater.DEFAULT_COMPRESSION);
                boolean inserted = false;
                Enumeration<? extends ZipEntry> entries = zip.entries();
                while (entries.hasMoreElements()) {
                    ZipEntry old = entries.nextElement();
                    String name = old.getName();
                    if (isV1Signature(name)) continue;
                    if (name.equals(targetPath)) {
                        writeStoredAligned(zout, counting, targetPath, library, old.getTime());
                        inserted = true;
                        continue;
                    }
                    copyZipEntry(zip, old, zout, counting);
                }
                if (!inserted) writeStoredAligned(zout, counting, targetPath, library, System.currentTimeMillis());
                zout.finish();
            }
        }
        try (ZipFile check = new ZipFile(output)) {
            ZipEntry e = check.getEntry(targetPath);
            if (e == null || e.getSize() != library.length || e.getMethod() != ZipEntry.STORED) {
                throw new IOException("Patched APK archive verification failed.");
            }
            try (InputStream in = check.getInputStream(e)) {
                byte[] actual = readAll(in);
                if (!MessageDigest.isEqual(MessageDigest.getInstance("SHA-256").digest(actual),
                        MessageDigest.getInstance("SHA-256").digest(library))) {
                    throw new IOException("The inserted library did not pass byte-for-byte verification.");
                }
            }
            Enumeration<? extends ZipEntry> all = check.entries();
            while (all.hasMoreElements()) {
                ZipEntry e2 = all.nextElement();
                if (e2.getName().startsWith("lib/") && e2.getName().endsWith(".so") && e2.getMethod() != ZipEntry.STORED) {
                    throw new IOException("Native library is compressed in output: " + e2.getName());
                }
            }
        }
    }

    private void copyZipEntry(ZipFile zip, ZipEntry old, ZipOutputStream zout, CountingOutputStream counting) throws IOException {
        ZipEntry entry = new ZipEntry(old.getName());
        if (old.getTime() >= 0) entry.setTime(old.getTime());
        if (old.getComment() != null) entry.setComment(old.getComment());
        boolean nativeLib = old.getName().startsWith("lib/") && old.getName().endsWith(".so");
        boolean stored = nativeLib || old.getMethod() == ZipEntry.STORED;
        if (stored) {
            entry.setMethod(ZipEntry.STORED);
            entry.setSize(old.getSize());
            entry.setCompressedSize(old.getSize());
            entry.setCrc(old.getCrc());
            if (nativeLib) setAlignmentExtra(entry, counting);
            else if (old.getExtra() != null) entry.setExtra(old.getExtra());
        } else {
            entry.setMethod(ZipEntry.DEFLATED);
        }
        zout.putNextEntry(entry);
        try (InputStream in = zip.getInputStream(old)) {
            copy(in, zout);
        }
        zout.closeEntry();
    }

    private void writeStoredAligned(ZipOutputStream zout, CountingOutputStream counting,
                                    String name, byte[] data, long time) throws IOException {
        CRC32 crc = new CRC32();
        crc.update(data);
        ZipEntry entry = new ZipEntry(name);
        entry.setMethod(ZipEntry.STORED);
        entry.setSize(data.length);
        entry.setCompressedSize(data.length);
        entry.setCrc(crc.getValue());
        if (time > 0) entry.setTime(time);
        setAlignmentExtra(entry, counting);
        zout.putNextEntry(entry);
        zout.write(data);
        zout.closeEntry();
    }

    private void setAlignmentExtra(ZipEntry entry, CountingOutputStream counting) {
        byte[] filename = entry.getName().getBytes(StandardCharsets.UTF_8);
        long dataStartWithoutExtra = counting.getCount() + 30L + filename.length;
        int extraLength = (int) ((ALIGNMENT - (dataStartWithoutExtra % ALIGNMENT)) % ALIGNMENT);
        if (extraLength == 0) {
            entry.setExtra(new byte[0]);
            return;
        }
        if (extraLength < 4) extraLength += ALIGNMENT;
        int payload = extraLength - 4;
        byte[] extra = new byte[extraLength];
        extra[0] = (byte) 0xD9;
        extra[1] = 0x35;
        extra[2] = (byte) (payload & 0xff);
        extra[3] = (byte) ((payload >>> 8) & 0xff);
        entry.setExtra(extra);
    }

    private boolean isV1Signature(String name) {
        String upper = name.toUpperCase(Locale.ROOT);
        if (!upper.startsWith("META-INF/") || upper.indexOf('/', 9) >= 0) return false;
        String leaf = upper.substring(9);
        return leaf.equals("MANIFEST.MF") || leaf.endsWith(".SF") || leaf.endsWith(".RSA") ||
                leaf.endsWith(".DSA") || leaf.endsWith(".EC") || leaf.startsWith("SIG-");
    }

    private static class CountingOutputStream extends FilterOutputStream {
        private long count;
        CountingOutputStream(OutputStream out) { super(out); }
        long getCount() { return count; }
        @Override public void write(int b) throws IOException {
            out.write(b);
            count++;
        }
        @Override public void write(byte[] b, int off, int len) throws IOException {
            out.write(b, off, len);
            count += len;
        }
    }

    private void signAndVerify(File unsigned, File signed) throws Exception {
        File keyFile = new File(getFilesDir(), "patcher-signing-key.pk8");
        File certFile = new File(getFilesDir(), "patcher-signing-cert.der");
        if (!keyFile.exists() || !certFile.exists()) createSigningIdentity(keyFile, certFile);

        PrivateKey privateKey = KeyFactory.getInstance("RSA").generatePrivate(
                new PKCS8EncodedKeySpec(readAll(keyFile)));
        X509Certificate certificate;
        try (InputStream in = new FileInputStream(certFile)) {
            certificate = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(in);
        }
        List<X509Certificate> certs = Collections.singletonList(certificate);
        ApkSigner.SignerConfig config = new ApkSigner.SignerConfig.Builder("patch-apk-tool", privateKey, certs).build();
        new ApkSigner.Builder(Collections.singletonList(config))
                .setInputApk(unsigned)
                .setOutputApk(signed)
                .setMinSdkVersion(23)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .setCreatedBy("Patch APK Tool")
                .build()
                .sign();

        ApkVerifier.Result result = new ApkVerifier.Builder(signed).build().verify();
        if (!result.isVerified()) throw new IOException("APK signature verification failed.");
    }

    private void createSigningIdentity(File keyFile, File certFile) throws Exception {
        if (Security.getProvider("BC") != null) Security.removeProvider("BC");
        Security.addProvider(new BouncyCastleProvider());
        KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA");
        generator.initialize(3072, new SecureRandom());
        KeyPair pair = generator.generateKeyPair();

        long now = System.currentTimeMillis();
        Date start = new Date(now - 24L * 60L * 60L * 1000L);
        Date end = new Date(now + 30L * 365L * 24L * 60L * 60L * 1000L);
        X500Name subject = new X500Name("CN=Patch APK Tool, O=Sampota, C=PH");
        BigInteger serial = new BigInteger(160, new SecureRandom()).abs().add(BigInteger.ONE);
        JcaX509v3CertificateBuilder builder = new JcaX509v3CertificateBuilder(
                subject, serial, start, end, subject, pair.getPublic());
        ContentSigner signer = new JcaContentSignerBuilder("SHA256withRSA")
                .setProvider("BC").build(pair.getPrivate());
        X509CertificateHolder holder = builder.build(signer);
        X509Certificate certificate = new JcaX509CertificateConverter()
                .setProvider("BC").getCertificate(holder);
        certificate.verify(pair.getPublic());

        try (FileOutputStream out = new FileOutputStream(keyFile)) {
            out.write(pair.getPrivate().getEncoded());
        }
        try (FileOutputStream out = new FileOutputStream(certFile)) {
            out.write(certificate.getEncoded());
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        executor.shutdown();
    }
}
