package com.anytimechat;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * Runtime permission requests for the BLE radio.
 *
 * <p>Java rather than C++ because this is an OS API with no Qt wrapper that covers the
 * Android 12 Bluetooth split — one of the two things the one-language rule makes an
 * exception for (docs/ARCHITECTURE.md section 01).
 *
 * <p><b>The permission set changes at API 31</b>, and getting this wrong is silent:
 * scanning simply returns nothing rather than raising an error.
 *
 * <ul>
 *   <li>API 31+ — BLUETOOTH_SCAN, BLUETOOTH_ADVERTISE, BLUETOOTH_CONNECT. The manifest
 *       declares {@code neverForLocation} on the scan permission, which is why a chat
 *       app can avoid asking for location at all.
 *   <li>API 28-30 — ACCESS_FINE_LOCATION, because the old model had no way to scan
 *       without it. There is no flag that changes that on those releases.
 * </ul>
 *
 * <p>POST_NOTIFICATIONS (API 33+) is requested in the same prompt, but deliberately NOT
 * required: someone who declines notifications must still be able to chat nearby. It
 * only affects whether the foreground-service notification is visible.
 */
public final class PermissionBridge {

    private static final String TAG = "AnyTimePermissions";
    private static final int REQUEST_CODE = 4711;
    private static final int NEARBY_REQUEST = 4712;

    private PermissionBridge() {}

    /** @return true when every permission the radio cannot work without is granted. */
    public static boolean hasBluetoothPermissions(Context context) {
        for (String permission : radioPermissions()) {
            if (context.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
                return false;
            }
        }
        return true;
    }

    /**
     * Prompts for anything still missing. Returns immediately; the core re-checks
     * {@link #hasBluetoothPermissions} on a timer rather than waiting for a result.
     *
     * @param context must be the Activity — only an Activity can show the dialog.
     */
    public static void requestBluetoothPermissions(Context context) {
        if (!(context instanceof Activity)) {
            Log.w(TAG, "cannot prompt for permissions without an Activity");
            return;
        }
        final Activity activity = (Activity) context;

        List<String> wanted = new ArrayList<>();
        for (String permission : radioPermissions()) wanted.add(permission);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {  // API 33
            wanted.add(Manifest.permission.POST_NOTIFICATIONS);
        }

        final List<String> missing = new ArrayList<>();
        for (String permission : wanted) {
            if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
                missing.add(permission);
            }
        }
        if (missing.isEmpty()) return;

        Log.i(TAG, "requesting " + missing.size() + " permission(s)");
        // The call arrives from the Qt thread; the dialog belongs on the UI thread.
        activity.runOnUiThread(
                () -> activity.requestPermissions(missing.toArray(new String[0]), REQUEST_CODE));
    }

    // ── Nearby Connections ──────────────────────────────────────────────────
    //
    // REQUESTED LAZILY, on the first photo — NOT at launch. Below API 33 Nearby
    // requires ACCESS_FINE_LOCATION outright, with no neverForLocation escape,
    // so asking at launch would put a location prompt in front of every user on
    // API 31-32 before they have any idea why. Asking when they first attach a
    // photo makes the reason obvious and makes declining cost them only fast
    // photos: BLE still carries the image, just slowly.

    /** @return true when Nearby can be used without another prompt. */
    public static boolean hasNearbyPermissions(Context context) {
        for (String permission : nearbyPermissions()) {
            if (context.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
                return false;
            }
        }
        return true;
    }

    /** Prompts for whatever Nearby still needs. Call on first photo, not at start-up. */
    public static void requestNearbyPermissions(Context context) {
        if (!(context instanceof Activity)) {
            Log.w(TAG, "cannot prompt for Nearby permissions without an Activity");
            return;
        }
        final Activity activity = (Activity) context;

        final List<String> missing = new ArrayList<>();
        for (String permission : nearbyPermissions()) {
            if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
                missing.add(permission);
            }
        }
        if (missing.isEmpty()) return;

        Log.i(TAG, "requesting " + missing.size() + " Nearby permission(s)");
        activity.runOnUiThread(
                () -> activity.requestPermissions(missing.toArray(new String[0]), NEARBY_REQUEST));
    }

    private static String[] nearbyPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {  // API 33
            // The whole reason the manifest caps location at 32: from here on
            // NEARBY_WIFI_DEVICES replaces it, and carries neverForLocation.
            return new String[] {Manifest.permission.NEARBY_WIFI_DEVICES};
        }
        return new String[] {Manifest.permission.ACCESS_FINE_LOCATION};
    }

    // ── Battery optimisation ────────────────────────────────────────────────
    //
    // DELIBERATELY NOT PROMPTED AUTOMATICALLY. Google Play prohibits asking for a
    // Doze exemption unless the app's core function is adversely affected without
    // one, and names foreground services as the preferred alternative — which is
    // exactly what MeshForegroundService already is, with the connectedDevice type
    // that exempts it from most background restrictions.
    //
    // So the app does not beg for this on launch. These two methods exist so a
    // settings screen can SHOW the state and let the user opt in themselves, which
    // is the variant that carries no policy risk. Wire them to a UI row, never to
    // start-up.

    /** @return true when this app is already exempt from Doze. */
    public static boolean isIgnoringBatteryOptimizations(Context context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return true;
        PowerManager power = context.getSystemService(PowerManager.class);
        if (power == null) return true;
        return power.isIgnoringBatteryOptimizations(context.getPackageName());
    }

    /**
     * Opens the system battery-optimisation list. Deliberately the SETTINGS screen
     * rather than ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS: the direct prompt is
     * the form Play restricts, and sending the user to the list they control is
     * both allowed and less likely to be dismissed reflexively.
     *
     * <p>Call only from an explicit user action.
     */
    public static void openBatteryOptimizationSettings(Context context) {
        try {
            Intent intent = new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
        } catch (RuntimeException e) {
            // Some OEM builds ship without this screen. Nothing to fall back to,
            // and it is not worth crashing over.
            Log.w(TAG, "battery optimisation settings unavailable", e);
        }
    }

    private static String[] radioPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {  // API 31
            return new String[] {
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_ADVERTISE,
                    Manifest.permission.BLUETOOTH_CONNECT,
            };
        }
        // Pre-31 the only way to scan was to hold location permission. The manifest
        // caps this at maxSdkVersion 30 so modern installs never see the prompt.
        return new String[] {Manifest.permission.ACCESS_FINE_LOCATION};
    }
}
