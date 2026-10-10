package com.anytimechat.nearby;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.util.Log;

import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailability;
import com.google.android.gms.nearby.Nearby;
import com.google.android.gms.nearby.connection.AdvertisingOptions;
import com.google.android.gms.nearby.connection.ConnectionInfo;
import com.google.android.gms.nearby.connection.ConnectionLifecycleCallback;
import com.google.android.gms.nearby.connection.ConnectionResolution;
import com.google.android.gms.nearby.connection.ConnectionsClient;
import com.google.android.gms.nearby.connection.DiscoveredEndpointInfo;
import com.google.android.gms.nearby.connection.DiscoveryOptions;
import com.google.android.gms.nearby.connection.EndpointDiscoveryCallback;
import com.google.android.gms.nearby.connection.Payload;
import com.google.android.gms.nearby.connection.PayloadCallback;
import com.google.android.gms.nearby.connection.PayloadTransferUpdate;
import com.google.android.gms.nearby.connection.Strategy;

import java.util.ArrayList;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The Nearby Connections dumb pipe — the BULK transport for photos.
 *
 * <p>Same contract as {@code BleRadio}: nine methods down, five callbacks up, and it never
 * inspects a byte it carries. No framing, no TTL, no fragmentation, no Noise, no announce
 * scheduling. All of that is core/protocol, written once (docs/ARCHITECTURE.md section 11).
 *
 * <p><b>THIS IS SECONDARY, NOT PRIMARY.</b> BLE carries discovery, presence, handshakes and
 * text; this carries images. Three reasons that split is not arbitrary:
 *
 * <ul>
 *   <li><b>One Strategy per app.</b> Nearby permits a single {@link Strategy} at a time.
 *       Discovery across many peers wants P2P_CLUSTER; bulk transfer wants
 *       P2P_POINT_TO_POINT, which has the highest throughput of the three. Choosing CLUSTER
 *       to be a general-purpose mesh would cap photo transfer at the SLOWEST strategy and
 *       throw away the only reason to be here at all. So: POINT_TO_POINT, bulk only.
 *   <li><b>Play Services.</b> This class cannot run without it. Making it primary would mean
 *       a de-Googled phone discovers nobody; as secondary, such a phone merely sends photos
 *       slowly over BLE. {@link #isAvailable} is what the C++ side checks before registering
 *       the transport at all.
 *   <li><b>Battery.</b> Nearby discovery drives BLE, classic Bluetooth and Wi-Fi scanning at
 *       once. The core only ever asks for it in DiscoveryMode.Active — someone actually
 *       looking at the Nearby screen — never in the background.
 * </ul>
 *
 * <p><b>Why connections are auto-accepted.</b> {@code onConnectionInitiated} offers a
 * verification code meant to be compared by two humans. We accept without showing it, and
 * that is deliberate rather than lazy: the authentication that matters is the Noise XX
 * handshake the core runs INSIDE this pipe, which pins the peer's static key
 * (section 08). Nearby's channel is treated as untrusted transport, exactly like BLE — so
 * adding a second, weaker human-comparison step would imply a guarantee we do not rely on.
 *
 * <p><b>Threading.</b> Nearby delivers callbacks on the main looper. The native methods only
 * marshal and enqueue; NearbyConnectionsTransport hops to the core thread before anything
 * touches core state, exactly as the BLE binding does.
 *
 * <p><b>NOT YET VERIFIED ON HARDWARE.</b> Written against the documented API.
 */
@SuppressLint("MissingPermission")  // PermissionBridge gates start() on the C++ side
public final class NearbyRadio {

    private static final String TAG = "AnyTimeNearby";

    /**
     * Highest-throughput strategy, and the only sensible one for a transport that exists
     * to move photos. See the class comment for why not P2P_CLUSTER.
     */
    private static final Strategy STRATEGY = Strategy.P2P_POINT_TO_POINT;

    /**
     * Nearby caps a BYTES payload at roughly 1 MB. Our frames are far smaller — the core
     * fragments to caps().maxFramePayload — so this is a sanity bound, not a design limit.
     */
    private static final int MAX_FRAME_BYTES = 512 * 1024;

    private final Context context;
    private final boolean verbose;
    private final ConnectionsClient connections;
    private final AtomicInteger nextLinkId = new AtomicInteger(1);

    private String serviceId;
    /** Our peer id as hex — advertised as the endpoint name so peers can identify us. */
    private String localEndpointName;

    private volatile boolean started = false;
    private volatile boolean advertising = false;
    private volatile boolean discovering = false;

    /** endpointId -> linkId, and back. Nearby speaks opaque strings; the core speaks ints. */
    private final Map<String, Integer> linkByEndpoint = new ConcurrentHashMap<>();
    private final Map<Integer, String> endpointByLink = new ConcurrentHashMap<>();
    /** Endpoints whose connection is requested but not yet resolved. */
    private final Map<String, byte[]> pendingPeerIds = new ConcurrentHashMap<>();

    public NearbyRadio(Context context) {
        this.context = context.getApplicationContext();
        this.verbose = (this.context.getApplicationInfo().flags
                        & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        this.connections = Nearby.getConnectionsClient(this.context);
    }

    private void debug(String message) {
        if (verbose) Log.d(TAG, message);
    }

    // ── called from C++ over JNI ─────────────────────────────────────────────

    /**
     * Whether this device can run Nearby at all.
     *
     * <p>Checked by C++ BEFORE the transport is registered, not after. That ordering
     * matters: TransportRegistry.allAvailable() requires every registered transport to be
     * usable, and AppCore retries transports on a timer until it is. Registering a
     * permanently unavailable Nearby on a de-Googled phone would spin that retry every
     * few seconds for the life of the process.
     */
    public static boolean isAvailable(Context context) {
        try {
            final int status = GoogleApiAvailability.getInstance()
                    .isGooglePlayServicesAvailable(context.getApplicationContext());
            return status == ConnectionResult.SUCCESS;
        } catch (RuntimeException e) {
            Log.w(TAG, "could not query Play Services availability", e);
            return false;
        }
    }

    /** @return true when the radio is usable. Idempotent. */
    public synchronized boolean start(String serviceIdText, byte[] peerId) {
        if (started) return true;

        if (!isAvailable(context)) {
            debug("Play Services unavailable; Nearby stays down");
            return false;
        }
        if (serviceIdText == null || serviceIdText.isEmpty() || peerId == null) return false;

        this.serviceId = serviceIdText;
        this.localEndpointName = toHex(peerId);
        started = true;

        Log.i(TAG, "nearby radio started");
        return true;
    }

    /** Idempotent. Tears down every endpoint; does not report state upward. */
    public synchronized void stop() {
        stopAdvertising();
        stopScanning();

        try {
            connections.stopAllEndpoints();
        } catch (RuntimeException e) {
            debug("stopAllEndpoints: " + e.getMessage());
        }

        linkByEndpoint.clear();
        endpointByLink.clear();
        pendingPeerIds.clear();

        if (started) Log.i(TAG, "nearby radio stopped");
        started = false;
    }

    public void startAdvertising() {
        if (!started || advertising) return;

        AdvertisingOptions options =
                new AdvertisingOptions.Builder().setStrategy(STRATEGY).build();

        advertising = true;
        connections.startAdvertising(localEndpointName, serviceId, connectionLifecycle, options)
                .addOnSuccessListener(ignored -> debug("advertising"))
                .addOnFailureListener(e -> {
                    advertising = false;
                    // Not reported as the radio going down: discovery and open links still
                    // work, and STATUS_ALREADY_ADVERTISING is benign.
                    Log.w(TAG, "nearby advertising failed", e);
                });
    }

    public void stopAdvertising() {
        if (!advertising) return;
        try {
            connections.stopAdvertising();
        } catch (RuntimeException e) {
            debug("stopAdvertising: " + e.getMessage());
        }
        advertising = false;
    }

    /**
     * Nearby discovery. The core calls this ONLY for DiscoveryMode.Active — see the class
     * comment; running it in the background would drive three radios at once.
     */
    public void startScanning() {
        if (!started || discovering) return;

        DiscoveryOptions options =
                new DiscoveryOptions.Builder().setStrategy(STRATEGY).build();

        discovering = true;
        connections.startDiscovery(serviceId, endpointDiscovery, options)
                .addOnSuccessListener(ignored -> debug("discovering"))
                .addOnFailureListener(e -> {
                    discovering = false;
                    Log.w(TAG, "nearby discovery failed", e);
                });
    }

    public void stopScanning() {
        if (!discovering) return;
        try {
            connections.stopDiscovery();
        } catch (RuntimeException e) {
            debug("stopDiscovery: " + e.getMessage());
        }
        discovering = false;
    }

    /**
     * Requests a connection to a discovered endpoint.
     *
     * @param endpointId the opaque id from onEndpointFound, carried up as PeerHandle.address
     * @return a link id, or 0 when the request could not be started
     */
    public int openLink(String endpointId) {
        if (!started || endpointId == null) return 0;

        Integer existing = linkByEndpoint.get(endpointId);
        if (existing != null) return existing;

        final int linkId = nextLinkId.getAndIncrement();
        // Registered before requestConnection: the lifecycle callback can arrive before
        // the call returns, and must find the link.
        linkByEndpoint.put(endpointId, linkId);
        endpointByLink.put(linkId, endpointId);

        connections.requestConnection(localEndpointName, endpointId, connectionLifecycle)
                .addOnFailureListener(e -> {
                    Log.w(TAG, "nearby connection request failed", e);
                    forgetEndpoint(endpointId);
                    onLinkDown(linkId, 3);  // Error
                });

        debug("link " + linkId + ": requesting connection to " + endpointId);
        return linkId;
    }

    public void closeLink(int linkId) {
        String endpointId = endpointByLink.get(linkId);
        if (endpointId == null) return;

        forgetEndpoint(endpointId);
        try {
            connections.disconnectFromEndpoint(endpointId);
        } catch (RuntimeException e) {
            debug("disconnectFromEndpoint: " + e.getMessage());
        }

        debug("link " + linkId + ": closed locally");
        onLinkDown(linkId, 0);  // Local
    }

    /**
     * Sends one complete frame.
     *
     * <p>No chunking here, unlike BleRadio: Nearby does its own reliable segmentation and
     * reassembly, so a BYTES payload arrives whole or not at all. That is the entire
     * reason this transport advertises a large maxFramePayload — the core fragments far
     * less, and a photo becomes tens of frames instead of a thousand.
     */
    public boolean send(int linkId, byte[] frame) {
        if (frame == null || frame.length == 0 || frame.length > MAX_FRAME_BYTES) return false;

        String endpointId = endpointByLink.get(linkId);
        if (endpointId == null) return false;

        try {
            connections.sendPayload(endpointId, Payload.fromBytes(frame));
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "nearby sendPayload threw", e);
            return false;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────

    private void forgetEndpoint(String endpointId) {
        Integer linkId = linkByEndpoint.remove(endpointId);
        if (linkId != null) endpointByLink.remove(linkId);
        pendingPeerIds.remove(endpointId);
    }

    /** Endpoint names are peer-id hex. A malformed one means a build we do not know. */
    private static byte[] peerIdFromName(String endpointName) {
        if (endpointName == null || endpointName.length() != 16) return null;
        byte[] out = new byte[8];
        try {
            for (int i = 0; i < 8; i++) {
                out[i] = (byte) Integer.parseInt(endpointName.substring(i * 2, i * 2 + 2), 16);
            }
        } catch (NumberFormatException e) {
            return null;
        }
        return out;
    }

    private static String toHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) sb.append(String.format("%02x", b));
        return sb.toString();
    }

    // ── Nearby callbacks — main looper ───────────────────────────────────────

    private final EndpointDiscoveryCallback endpointDiscovery = new EndpointDiscoveryCallback() {
        @Override
        public void onEndpointFound(String endpointId, DiscoveredEndpointInfo info) {
            if (!serviceId.equals(info.getServiceId())) return;

            byte[] peerId = peerIdFromName(info.getEndpointName());
            if (peerId == null) {
                debug("endpoint " + endpointId + " has an unrecognised name; ignored");
                return;
            }
            pendingPeerIds.put(endpointId, peerId);

            // RSSI 0 = SignalBand.Unknown. Nearby does not expose signal strength, so the
            // Nearby screen shows these peers without a strength band rather than
            // inventing one. BLE sightings of the same person still carry real RSSI.
            onPeerSeen(peerId, endpointId, 0);
        }

        @Override
        public void onEndpointLost(String endpointId) {
            debug("endpoint lost: " + endpointId);
            // Deliberately NOT reported as peer.lost: liveness comes from signed ANNOUNCE
            // packets inside the 60 s window, never from transport events (section 11).
            pendingPeerIds.remove(endpointId);
        }
    };

    private final ConnectionLifecycleCallback connectionLifecycle =
            new ConnectionLifecycleCallback() {

        @Override
        public void onConnectionInitiated(String endpointId, ConnectionInfo info) {
            // Accepted without showing info.getAuthenticationDigits() — see the class
            // comment. Noise inside this pipe is what authenticates the peer.
            byte[] peerId = peerIdFromName(info.getEndpointName());
            if (peerId != null) pendingPeerIds.put(endpointId, peerId);

            if (!linkByEndpoint.containsKey(endpointId)) {
                // Incoming: they requested us. Allocate a link now.
                final int linkId = nextLinkId.getAndIncrement();
                linkByEndpoint.put(endpointId, linkId);
                endpointByLink.put(linkId, endpointId);
            }

            connections.acceptConnection(endpointId, payloadCallback)
                    .addOnFailureListener(e -> {
                        Log.w(TAG, "could not accept nearby connection", e);
                        forgetEndpoint(endpointId);
                    });
        }

        @Override
        public void onConnectionResult(String endpointId, ConnectionResolution resolution) {
            Integer linkId = linkByEndpoint.get(endpointId);
            if (linkId == null) return;

            if (resolution.getStatus().isSuccess()) {
                byte[] peerId = pendingPeerIds.get(endpointId);
                debug("link " + linkId + ": connected");
                // An unknown name yields eight zero bytes, and the core binds the link to
                // a peer when its first signed ANNOUNCE arrives — the same path incoming
                // BLE links already take (TransportRegistry.bindLink).
                onLinkUp(linkId, peerId != null ? peerId : new byte[8]);
            } else {
                debug("link " + linkId + ": connection refused, "
                      + resolution.getStatus().getStatusCode());
                forgetEndpoint(endpointId);
                onLinkDown(linkId, 3);  // Error
            }
        }

        @Override
        public void onDisconnected(String endpointId) {
            Integer linkId = linkByEndpoint.get(endpointId);
            if (linkId == null) return;

            forgetEndpoint(endpointId);
            debug("link " + linkId + ": remote disconnected");
            onLinkDown(linkId, 1);  // Remote
        }
    };

    private final PayloadCallback payloadCallback = new PayloadCallback() {
        @Override
        public void onPayloadReceived(String endpointId, Payload payload) {
            Integer linkId = linkByEndpoint.get(endpointId);
            if (linkId == null) return;

            // BYTES only. A FILE or STREAM payload is not something this protocol sends,
            // so one arriving means a peer running a different build — ignore it rather
            // than guess at its framing.
            if (payload.getType() != Payload.Type.BYTES) {
                Log.w(TAG, "ignoring a non-BYTES nearby payload");
                return;
            }

            byte[] frame = payload.asBytes();
            if (frame != null && frame.length > 0) onFrame(linkId, frame);
        }

        @Override
        public void onPayloadTransferUpdate(String endpointId, PayloadTransferUpdate update) {
            // BYTES payloads complete in one shot, so there is no progress worth
            // reporting. Kept because PayloadCallback requires it.
        }
    };

    // ── native callbacks, implemented in adapters/transport/nearby ───────────
    // These ENQUEUE onto the core thread and return immediately.

    private static native void onPeerSeen(byte[] peerId, String endpointId, int rssi);

    private static native void onLinkUp(int linkId, byte[] peerId);

    private static native void onLinkDown(int linkId, int reason);

    private static native void onFrame(int linkId, byte[] frame);

    private static native void onRadioState(int state);

    /** Silences the unused-method warning for a callback the C++ side invokes on teardown. */
    static void reportRadioState(int state) {
        onRadioState(state);
    }

    /** Kept so ArrayList stays referenced if a future change needs bulk endpoint walks. */
    @SuppressWarnings("unused")
    private static ArrayList<String> unusedPlaceholder() {
        return new ArrayList<>();
    }
}
