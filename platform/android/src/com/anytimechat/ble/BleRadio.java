package com.anytimechat.ble;

import android.content.Context;

/**
 * The entire Java surface of the radio: nine methods down, five callbacks up.
 *
 * <p><b>THE DUMB PIPE RULE</b> (docs/ARCHITECTURE.md §11). This class advertises,
 * scans, opens and closes GATT links, and moves opaque byte arrays. It knows
 * nothing about what those bytes mean. There is no framing here, no TTL, no
 * fragmentation, no Noise, no announce scheduling — all of that is core/protocol
 * in C++, written once and shared with the iOS port.
 *
 * <p>The iOS class will have <em>the same nine methods and the same five
 * callbacks</em>, backed by CBPeripheralManager and CBCentralManager. That
 * symmetry is the deliverable. If this file starts growing methods the Swift one
 * lacks, protocol logic has leaked downward and the iOS port just got expensive.
 *
 * <p><b>THREADING.</b> Android delivers BLE callbacks on binder threads. The C++
 * core is single-threaded. The native methods below therefore only ENQUEUE —
 * BleGattTransport hops to the core thread before touching any core state. Never
 * call into core logic directly from a callback here.
 *
 * <p>Fragmentation deliberately does not live here even though MTU does. The
 * frame size is reported upward through TransportCapabilities and the core does
 * the splitting; doing it here would mean writing it again in Swift.
 */
public final class BleRadio {

    static { System.loadLibrary("anytime"); }

    private final Context context;

    public BleRadio(Context context) {
        this.context = context.getApplicationContext();
    }

    // ── called from C++ over JNI ─────────────────────────────────────────────

    /**
     * @param serviceUuid must match protocol::kServiceUuid
     * @param peerId      8 bytes, placed in the advertising payload alongside the
     *                    service UUID. Android caps adverts at 29 usable bytes,
     *                    so nickname and public keys travel in ANNOUNCE instead.
     */
    public void start(String serviceUuid, byte[] peerId) {
        throw new UnsupportedOperationException("phase 2");
    }

    public void stop() {
        throw new UnsupportedOperationException("phase 2");
    }

    public void startAdvertising() {
        throw new UnsupportedOperationException("phase 2");
    }

    public void stopAdvertising() {
        throw new UnsupportedOperationException("phase 2");
    }

    public void startScanning() {
        throw new UnsupportedOperationException("phase 2");
    }

    public void stopScanning() {
        throw new UnsupportedOperationException("phase 2");
    }

    /** @return a link id, or 0 when the connection could not be started. */
    public int openLink(String deviceAddress) {
        throw new UnsupportedOperationException("phase 2");
    }

    public void closeLink(int linkId) {
        throw new UnsupportedOperationException("phase 2");
    }

    /** @param frame already sized to fit the reported frame budget by the core. */
    public boolean send(int linkId, byte[] frame) {
        throw new UnsupportedOperationException("phase 2");
    }

    // ── native callbacks, implemented in adapters/transport/ble ──────────────
    // These ENQUEUE onto the core thread and return immediately.

    private static native void onPeerSeen(byte[] peerId, String address, int rssi);

    private static native void onLinkUp(int linkId, byte[] peerId);

    private static native void onLinkDown(int linkId, int reason);

    private static native void onFrame(int linkId, byte[] frame);

    private static native void onRadioState(int state);
}
