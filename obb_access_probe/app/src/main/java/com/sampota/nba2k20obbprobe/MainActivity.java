package com.sampota.nba2k20obbprobe;

import android.Manifest;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.Locale;

public final class MainActivity extends Activity {
    private static final int REQUEST_READ_PERMISSION = 41;
    private static final String TAG = "OBB_ACCESS_PROBE";

    private TextView output;
    private String lastReport = "No check has been run yet.";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);

        TextView heading = new TextView(this);
        heading.setText("Read-only check of the original NBA 2K20 expansion files.\n"
                + "This app does not create, move, rename, write, or delete any files.");
        heading.setTextSize(16);
        root.addView(heading, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        Button run = new Button(this);
        run.setText("Run read-only OBB check");
        run.setOnClickListener(v -> requestPermissionOrRun());
        root.addView(run, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        Button copy = new Button(this);
        copy.setText("Copy report");
        copy.setOnClickListener(v -> copyReport());
        root.addView(copy, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        ScrollView scroll = new ScrollView(this);
        output = new TextView(this);
        output.setTextSize(13);
        output.setTextIsSelectable(true);
        output.setGravity(Gravity.START);
        output.setPadding(0, dp(12), 0, 0);
        scroll.addView(output);
        root.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
        output.setText(lastReport);
        requestPermissionOrRun();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void requestPermissionOrRun() {
        if (Build.VERSION.SDK_INT >= 23
                && checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            output.setText("Android needs the read-only Files/Media permission to check the OBB path.\n"
                    + "No write permission is requested. Tap Run again after granting it.");
            requestPermissions(
                    new String[]{Manifest.permission.READ_EXTERNAL_STORAGE},
                    REQUEST_READ_PERMISSION);
            return;
        }
        runReadOnlyCheck();
    }

    @Override
    public void onRequestPermissionsResult(
            int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_READ_PERMISSION) {
            if (grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                runReadOnlyCheck();
            } else {
                lastReport = "READ_EXTERNAL_STORAGE was not granted. No OBB files were opened.";
                output.setText(lastReport);
            }
        }
    }

    private void runReadOnlyCheck() {
        StringBuilder report = new StringBuilder();
        report.append("NBA 2K20 OBB ACCESS PROBE (READ ONLY)\n");
        report.append("No files are created, changed, moved, renamed, or deleted.\n\n");
        report.append("Device Android: ").append(Build.VERSION.RELEASE)
                .append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        report.append("Probe package: ").append(getPackageName()).append("\n");
        report.append("READ_EXTERNAL_STORAGE granted: ")
                .append(Build.VERSION.SDK_INT < 23
                        || checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                        == PackageManager.PERMISSION_GRANTED).append("\n");
        report.append("External storage state: ")
                .append(Environment.getExternalStorageState()).append("\n");
        if (Build.VERSION.SDK_INT >= 29) {
            report.append("Legacy external-storage mode: ")
                    .append(Environment.isExternalStorageLegacy()).append("\n");
        }

        try {
            File ownObb = getObbDir();
            report.append("Probe app getObbDir(): ")
                    .append(ownObb == null ? "null" : ownObb.getAbsolutePath()).append("\n");
        } catch (Throwable t) {
            report.append("Probe getObbDir() error: ").append(t).append("\n");
        }

        File external = Environment.getExternalStorageDirectory();
        File originalDir = new File(external,
                "Android/obb/com.t2ksports.nba2k20and");
        report.append("\nOriginal game OBB directory: ")
                .append(originalDir.getAbsolutePath()).append("\n");
        report.append("Directory exists: ").append(originalDir.exists())
                .append(", isDirectory: ").append(originalDir.isDirectory())
                .append(", canRead: ").append(originalDir.canRead()).append("\n");

        inspectFile(report, new File(originalDir,
                "main.98.com.t2ksports.nba2k20and.obb"));
        inspectFile(report, new File(originalDir,
                "patch.98.com.t2ksports.nba2k20and.obb"));

        File downloadCopyDir = new File(external,
                "Download/com.t2ksports.nba2k20and");
        report.append("\\nUser-reported Download copy directory: ")
                .append(downloadCopyDir.getAbsolutePath()).append("\\n");
        report.append("Directory exists: ").append(downloadCopyDir.exists())
                .append(", isDirectory: ").append(downloadCopyDir.isDirectory())
                .append(", canRead: ").append(downloadCopyDir.canRead()).append("\\n");
        if (downloadCopyDir.isDirectory()) {
            File[] entries = downloadCopyDir.listFiles();
            if (entries == null) {
                report.append("Directory listing: FAILED or denied\\n");
            } else {
                report.append("Directory entries: ").append(entries.length).append("\\n");
                for (File entry : entries) {
                    report.append(" - ").append(entry.getName())
                            .append(" | file=").append(entry.isFile())
                            .append(" | dir=").append(entry.isDirectory())
                            .append(" | bytes=").append(entry.isFile() ? entry.length() : -1)
                            .append(" | canRead=").append(entry.canRead()).append("\\n");
                }
                inspectFile(report, new File(downloadCopyDir,
                        "main.98.com.t2ksports.nba2k20and.obb"));
                inspectFile(report, new File(downloadCopyDir,
                        "patch.98.com.t2ksports.nba2k20and.obb"));
            }
        }

        lastReport = report.toString();
        output.setText(lastReport);
        Log.i(TAG, lastReport);
    }

    private void inspectFile(StringBuilder report, File file) {
        report.append("\nFile: ").append(file.getName()).append("\n");
        report.append("Exists: ").append(file.exists())
                .append(", isFile: ").append(file.isFile())
                .append(", canRead: ").append(file.canRead()).append("\n");
        if (!file.exists()) {
            report.append("Open test: NOT RUN (file not found)\n");
            return;
        }
        report.append("Length: ").append(file.length()).append(" bytes\n");
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] signature = new byte[4];
            int count = input.read(signature);
            report.append("Open test: SUCCESS; bytes read: ").append(count);
            if (count > 0) {
                report.append("; first bytes: ");
                for (int i = 0; i < count; i++) {
                    report.append(String.format(Locale.US, "%02X", signature[i] & 0xff));
                    if (i + 1 < count) report.append(' ');
                }
            }
            report.append("\n");
        } catch (SecurityException | IOException e) {
            report.append("Open test: FAILED; ").append(e.getClass().getSimpleName())
                    .append(": ").append(e.getMessage()).append("\n");
        } catch (Throwable t) {
            report.append("Open test: FAILED; ").append(t).append("\n");
        }
    }

    private void copyReport() {
        ClipboardManager clipboard =
                (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            Toast.makeText(this, "Clipboard unavailable", Toast.LENGTH_SHORT).show();
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("OBB access report", lastReport));
        Toast.makeText(this, "Report copied", Toast.LENGTH_SHORT).show();
    }
}
