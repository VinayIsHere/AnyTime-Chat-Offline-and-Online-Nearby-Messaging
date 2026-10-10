package com.anytimechat;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.content.Intent;
import android.provider.Settings;
import android.util.Log;

/**
 * Switching the Bluetooth radio ON, as opposed to sending bytes over it.
 *
 * <p>Java rather than C++ for the same reason {@link PermissionBridge} is: the Android 12
 * Bluetooth split and {@code ACTION_REQUEST_ENABLE} have no Qt wrapper, and Qt Bluetooth is
 * deliberately not linked (AD-02). Static methods called over JNI, mirroring PermissionBridge
 * exactly — there is no JavaScript bridge here and there must not be one, because the page
 * already reaches the core over loopback HTTP and a second path in would be a weaker one
 * (see AnyTimeActivity).
 *
 * <p><b>Nothing here reports state upward.</b> The core learns the radio came back the way it
 * always has: AppCore retries every transport every kTransportRetryMillis, BleRadio.start()
 * succeeds once the adapter is on, and that surfaces as an ordinary radio.state event. Adding
 * a second notification path would mean two sources of truth for one fact.
 */
public final class BluetoothBridge {

    private static final String TAG = "AnyTimeBluetooth";

    /** Mirrors anytime::RadioAvailability. Keep the numbers in step with Enums.h. */
    public static final int UNSUPPORTED  = 1;
    public static final int UNAUTHORIZED = 2;
    public static final int OFF          = 3;
    public static final int ON           = 4;

    /** Distinct from PermissionBridge's 4711/4712 and the file chooser's 4713. */
    private static final int ENABLE_REQUEST = 4714;

    private BluetoothBridge() {}

    /**
     * @return one of the constants above; never throws.
     */
    public static int state(Context context) {
        BluetoothAdapter adapter = adapterFor(context);
        if (adapter == null) return UNSUPPORTED;

        // PERMISSIONS ARE CHECKED BEFORE THE SWITCH, and not as a formality.
        // From API 31 the enable dialog itself requires BLUETOOTH_CONNECT, so
        // while the permission is missing there is nothing the user can usefully
        // do about the switch — reporting "off" would put a button in front of
        // them that throws when tapped.
        if (!PermissionBridge.hasBluetoothPermissions(context)) return UNAUTHORIZED;

        try {
            return adapter.isEnabled() ? ON : OFF;
        } catch (SecurityException e) {
            // Belt and braces: an OEM build that gates isEnabled() differently.
            Log.w(TAG, "not permitted to read the adapter state", e);
            return UNAUTHORIZED;
        }
    }

    /** Android can show the system enable dialog; iOS, when it exists, cannot. */
    public static boolean canPromptInApp() {
        return true;
    }

    /**
     * ONE BUTTON, TWO MEANINGS.
     *
     * <p>A missing permission and a switched-off radio look identical from the gate screen,
     * and they need opposite actions — asking to enable Bluetooth without BLUETOOTH_CONNECT
     * throws on API 31+. So whichever is actually missing is what gets prompted for.
     *
     * <p>Returns as soon as the prompt is raised. The result is never read back here: the
     * core notices through its ordinary transport retry.
     */
    public static void requestEnable(Context context) {
        BluetoothAdapter adapter = adapterFor(context);
        if (adapter == null) {
            Log.w(TAG, "no Bluetooth adapter to enable");
            return;
        }

        if (!PermissionBridge.hasBluetoothPermissions(context)) {
            PermissionBridge.requestBluetoothPermissions(context);
            return;
        }

        try {
            if (adapter.isEnabled()) return;  // already on; nothing to ask
        } catch (SecurityException ignored) {
            // Fall through and let the dialog decide.
        }

        if (!(context instanceof Activity)) {
            Log.w(TAG, "cannot show the enable dialog without an Activity");
            return;
        }
        final Activity activity = (Activity) context;

        // The call arrives from the Qt thread; the dialog belongs on the UI thread.
        activity.runOnUiThread(() -> {
            try {
                // startActivityForResult, because ACTION_REQUEST_ENABLE is defined as a
                // result-returning intent. The result is deliberately ignored —
                // AnyTimeActivity forwards unknown request codes to super, and the radio
                // coming up is observed through the transport rather than through a
                // callback that only fires if the user stays in the app.
                activity.startActivityForResult(
                        new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), ENABLE_REQUEST);
            } catch (SecurityException e) {
                Log.w(TAG, "not permitted to ask for Bluetooth to be enabled", e);
            } catch (RuntimeException e) {
                Log.w(TAG, "no activity could handle the Bluetooth enable request", e);
            }
        });
    }

    /**
     * Opens the system Bluetooth settings screen.
     *
     * <p>The fallback for every case the in-app dialog cannot cover: a permission the user
     * has permanently denied, an OEM build without the dialog, and iOS when that port lands.
     */
    public static void openSettings(Context context) {
        try {
            Intent intent = new Intent(Settings.ACTION_BLUETOOTH_SETTINGS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
        } catch (RuntimeException e) {
            // Some OEM builds ship without this screen. There is nothing to fall
            // back to, and it is not worth crashing over.
            Log.w(TAG, "Bluetooth settings unavailable", e);
        }
    }

    private static BluetoothAdapter adapterFor(Context context) {
        BluetoothManager manager =
                (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null) return null;
        return manager.getAdapter();
    }
}
