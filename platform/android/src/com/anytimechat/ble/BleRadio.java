package com.anytimechat.ble;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattServer;
import android.bluetooth.BluetoothGattServerCallback;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Log;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The dumb pipe. Advertises, scans, opens GATT links, and moves opaque byte arrays.
 *
 * <p><b>WHAT THIS CLASS DOES NOT DO</b> (docs/ARCHITECTURE.md section 11): no framing
 * beyond link chunking, no TTL, no fragmentation of protocol packets, no Noise, no
 * announce scheduling, and no decision about WHEN to connect — the core makes that call.
 * It never inspects a byte it carries.
 *
 * <p><b>Why there is chunking here anyway.</b> The default ATT MTU is 23 bytes, leaving
 * 20 usable per write — far below the 180-byte frame the core hands down. So a frame is
 * split into MTU-sized chunks behind a 2-byte length prefix and reassembled before being
 * handed up. That is link framing, like the length prefix LoopbackTransport uses over TCP,
 * and it does not break the dumb-pipe rule: complete frames in, complete frames out.
 *
 * <p><b>Why writes are queued.</b> Android GATT runs one operation at a time per
 * connection. A second write issued before the first one's callback is refused, so
 * firing chunks back to back loses everything after the first. Each link therefore keeps
 * a queue and sends the next chunk only from the previous chunk's completion callback.
 *
 * <p><b>Both roles at once.</b> Every device advertises AND scans, so a link is either
 * outgoing (we are central: we write to their RX, subscribe to their TX) or incoming (we
 * are peripheral: they write to our RX, we notify on our TX). {@link Link} records which,
 * because the send path differs.
 *
 * <p><b>Threading.</b> Callbacks arrive on binder threads. The native methods only
 * enqueue; BleGattTransport hops to the core thread before anything touches core state.
 *
 * <p><b>Verified on hardware</b> for the basic path: two Android handsets discover each
 * other, connect, and exchange chat traffic. What is still unmeasured is behaviour under
 * load — concurrent advertise-plus-scan with several GATT links varies sharply by
 * chipset, which is what the phase 2 endurance spike exists to quantify.
 */
@SuppressLint("MissingPermission")  // permissions are checked by PermissionBridge before start()
public final class BleRadio {

    private static final String TAG = "AnyTimeBle";

    // No System.loadLibrary() here. Qt has loaded the native library long before C++
    // constructs this class, and BleGattTransport binds the natives with RegisterNatives.
    // Loading by name would also be wrong: Qt adds an ABI suffix to the library name
    // (libanytime_arm64-v8a.so), so loadLibrary("anytime") throws, class initialisation
    // fails, and the radio is unusable for the life of the process.

    // Randomly generated (v4) alongside kServiceUuid in ProtocolConstants.h. The
    // previous values were patterned placeholders. Changing any of the three is a
    // breaking change: an older install and a newer one become invisible to each
    // other, with no error to explain why.

    /** Characteristic peers WRITE to when sending us a frame. */
    private static final UUID RX_UUID = UUID.fromString("084343eb-dce4-4921-8db3-04a296319399");
    /** Characteristic we NOTIFY on when sending a frame to a connected central. */
    private static final UUID TX_UUID = UUID.fromString("7b526f42-1d6a-4109-8ef7-29a781461542");
    /** Standard Client Characteristic Configuration Descriptor. */
    private static final UUID CCCD_UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    /** Manufacturer id carrying our peer id in the scan response.
     *
     *  <p>0xFFFF is the SIG's "reserved for testing" value and stays that way: a real
     *  Company Identifier requires Bluetooth SIG membership, and we do not need one.
     *  Collision is not a practical concern because the SCAN FILTER matches our random
     *  128-bit service UUID first — this field is only read off results that already
     *  passed that filter. */
    private static final int MANUFACTURER_ID = 0xFFFF;

    private static final int LENGTH_PREFIX_BYTES = 2;
    private static final int MAX_FRAME_BYTES = 4096;   // a frame larger than this is nonsense
    private static final int DEFAULT_CHUNK = 20;       // ATT MTU 23 minus 3 bytes of header
    private static final int PREFERRED_MTU = 517;      // the maximum Android will negotiate

    /** Hard ceiling on one attribute write, from the Bluetooth Core Specification: an
     *  attribute value is at most 512 octets. MTU 517 minus the 3-byte ATT header is
     *  514, which EXCEEDS that — so chunk size is clamped here rather than trusting the
     *  arithmetic. Writing 514 bytes gets silently truncated or rejected depending on
     *  the peer stack, which would corrupt the frame stream. */
    private static final int MAX_ATT_VALUE = 512;

    /** Mirrors android.bluetooth.le.ScanSettings, passed down from the core's
     *  DiscoveryMode so the scan duty cycle is a core decision, not a radio one. */
    private static final int SCAN_MODE_LOW_POWER = 0;
    private static final int SCAN_MODE_LOW_LATENCY = 2;

    /** Chunks allowed to wait on one link. A link this far behind is not delivering, and
     *  refusing more lets the core's retry logic take over instead of buffering forever. */
    private static final int MAX_QUEUED_CHUNKS = 512;
    /** A refused write (stack busy) is retried with a growing delay, then the link is
     *  closed: a half-sent frame has already desynchronised the far end's stream. */
    private static final int MAX_WRITE_RETRIES = 5;
    private static final long WRITE_RETRY_DELAY_MS = 30;

    private final Context context;
    /** Debug-level logs only in debuggable builds. Warnings and errors always log. */
    private final boolean verbose;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final AtomicInteger nextLinkId = new AtomicInteger(1);

    private BluetoothAdapter adapter;
    private BluetoothLeAdvertiser advertiser;
    private BluetoothLeScanner scanner;
    private BluetoothGattServer gattServer;
    private BluetoothGattCharacteristic txCharacteristic;

    private UUID serviceUuid;
    private byte[] localPeerId;

    private volatile boolean started = false;
    private volatile boolean advertising = false;
    private volatile boolean scanning = false;
    /** The mode the live scan was started with, so a duty-cycle change restarts it. */
    private volatile int currentScanMode = SCAN_MODE_LOW_LATENCY;
    private boolean receiverRegistered = false;

    /** One open link, in either role. Mutable state is guarded by the Link's monitor. */
    private static final class Link {
        int linkId;
        BluetoothDevice device;
        byte[] peerId;
        /** Set when we are the central; null when we are the peripheral. */
        BluetoothGatt gatt;
        BluetoothGattCharacteristic remoteRx;
        int chunkSize = DEFAULT_CHUNK;

        /** Reassembly buffer for inbound chunks. */
        byte[] inbound = new byte[0];

        /** Chunks waiting to go out, oldest first. */
        final ArrayDeque<byte[]> outbound = new ArrayDeque<>();
        /** True while a chunk is in flight and its completion callback has not arrived. */
        boolean writing = false;
        /** Nothing is sent until the far side can receive: as central, once our subscription
         *  to its TX is confirmed; as peripheral, once the central has subscribed to ours. */
        boolean ready = false;
        int retries = 0;
    }

    private final Map<Integer, Link> links = new ConcurrentHashMap<>();
    private final Map<String, Integer> linkByAddress = new ConcurrentHashMap<>();

    public BleRadio(Context context) {
        this.context = context.getApplicationContext();
        this.verbose = (this.context.getApplicationInfo().flags
                        & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    private void debug(String message) {
        if (verbose) Log.d(TAG, message);
    }

    // ── called from C++ over JNI ─────────────────────────────────────────────

    /**
     * @return true when the radio is running. False means Bluetooth is off, missing, or
     *     refused us; the core retries on a timer, so nothing here needs to.
     */
    public synchronized boolean start(String serviceUuidText, byte[] peerId) {
        if (started) return true;

        try {
            this.serviceUuid = UUID.fromString(serviceUuidText);
        } catch (IllegalArgumentException e) {
            Log.e(TAG, "invalid service UUID: " + serviceUuidText);
            return false;
        }
        this.localPeerId = peerId.clone();

        BluetoothManager manager =
                (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null) {
            Log.e(TAG, "no BluetoothManager on this device");
            return false;
        }

        adapter = manager.getAdapter();
        if (adapter == null) {
            Log.e(TAG, "this device has no Bluetooth adapter");
            return false;
        }
        if (!adapter.isEnabled()) {
            debug("bluetooth is switched off");
            return false;
        }

        advertiser = adapter.getBluetoothLeAdvertiser();
        scanner = adapter.getBluetoothLeScanner();
        if (advertiser == null) {
            Log.w(TAG, "this device cannot advertise over BLE; others will not find it");
        }

        try {
            gattServer = manager.openGattServer(context, gattServerCallback);
        } catch (SecurityException e) {
            Log.e(TAG, "not permitted to open the GATT server", e);
            return false;
        }
        if (gattServer == null) {
            Log.e(TAG, "could not open the GATT server");
            return false;
        }
        publishService();

        registerAdapterReceiver();
        started = true;
        Log.i(TAG, "radio started");
        return true;
    }

    /** Idempotent. Does not report radio state upward — C++ asked for this and knows. */
    public synchronized void stop() {
        stopAdvertising();
        stopScanning();
        unregisterAdapterReceiver();

        for (Link link : new ArrayList<>(links.values())) closeLink(link.linkId);

        if (gattServer != null) {
            try {
                gattServer.close();
            } catch (RuntimeException e) {
                Log.w(TAG, "closing the GATT server failed", e);
            }
            gattServer = null;
        }
        txCharacteristic = null;

        if (started) Log.i(TAG, "radio stopped");
        started = false;
    }

    public void startAdvertising() {
        if (!started || advertising || advertiser == null) return;

        AdvertiseSettings settings = new AdvertiseSettings.Builder()
                .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_BALANCED)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)
                .setConnectable(true)
                .setTimeout(0)
                .build();

        // The 31-byte advertisement holds the 128-bit service UUID and little else,
        // so the peer id goes in the SCAN RESPONSE — a second 31 bytes Android
        // merges into the same ScanResult.
        AdvertiseData advertisement = new AdvertiseData.Builder()
                .setIncludeDeviceName(false)   // a device name would not fit beside the UUID
                .setIncludeTxPowerLevel(false)
                .addServiceUuid(new ParcelUuid(serviceUuid))
                .build();

        AdvertiseData scanResponse = new AdvertiseData.Builder()
                .addManufacturerData(MANUFACTURER_ID, localPeerId)
                .build();

        try {
            // Marked before the callback so a retry in the meantime does not start a
            // second advertisement; onStartFailure clears it.
            advertising = true;
            advertiser.startAdvertising(settings, advertisement, scanResponse,
                                        advertiseCallback);
            debug("advertising requested");
        } catch (RuntimeException e) {
            advertising = false;
            Log.e(TAG, "could not start advertising", e);
        }
    }

    public void stopAdvertising() {
        if (advertiser != null && advertising) {
            try {
                advertiser.stopAdvertising(advertiseCallback);
            } catch (RuntimeException e) {
                debug("stopAdvertising: " + e.getMessage());  // throws once BT is off
            }
        }
        advertising = false;
    }

    /**
     * @param androidScanMode one of the SCAN_MODE_* values above. LOW_LATENCY scans
     *     continuously and is only justified while the user is watching the Nearby
     *     screen; LOW_POWER duty-cycles in the background, which is what keeps the mesh
     *     reachable overnight without flattening the battery.
     */
    public void startScanning(int androidScanMode) {
        if (!started || scanner == null) return;

        if (scanning) {
            // Already scanning the way the core wants — nothing to do. Android has no
            // "change the mode" call, so a genuine change means stop and start again.
            if (currentScanMode == androidScanMode) return;
            debug("scan mode " + currentScanMode + " -> " + androidScanMode);
            stopScanning();
        }

        List<ScanFilter> filters = Collections.singletonList(
                new ScanFilter.Builder()
                        .setServiceUuid(new ParcelUuid(serviceUuid))
                        .build());

        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(androidScanMode)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
                .build();

        try {
            scanner.startScan(filters, settings, scanCallback);
            scanning = true;
            currentScanMode = androidScanMode;
            debug("scanning started, mode " + androidScanMode);
        } catch (RuntimeException e) {
            Log.e(TAG, "could not start scanning", e);
        }
    }

    public void stopScanning() {
        if (scanner != null && scanning) {
            try {
                scanner.stopScan(scanCallback);
            } catch (RuntimeException e) {
                debug("stopScan: " + e.getMessage());
            }
        }
        scanning = false;
    }

    /** @return a link id, or 0 when the connection could not be started. */
    public int openLink(String deviceAddress) {
        if (!started || adapter == null) return 0;

        Integer existing = linkByAddress.get(deviceAddress);
        if (existing != null) return existing;

        BluetoothDevice device;
        try {
            device = adapter.getRemoteDevice(deviceAddress);
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "bad device address");
            return 0;
        }

        Link link = new Link();
        link.linkId = nextLinkId.getAndIncrement();
        link.device = device;

        // Registered before connectGatt: the connection callback can arrive before
        // connectGatt returns, and must find the link.
        links.put(link.linkId, link);
        linkByAddress.put(deviceAddress, link.linkId);

        try {
            // TRANSPORT_LE explicitly: without it a dual-mode device may be reached
            // over BR/EDR, where our GATT service does not exist.
            link.gatt = device.connectGatt(context, false, gattClientCallback,
                                           BluetoothDevice.TRANSPORT_LE);
        } catch (RuntimeException e) {
            Log.w(TAG, "connectGatt threw", e);
            link.gatt = null;
        }
        if (link.gatt == null) {
            links.remove(link.linkId);
            linkByAddress.remove(deviceAddress);
            return 0;
        }

        debug("link " + link.linkId + ": connecting as central");
        return link.linkId;
    }

    public void closeLink(int linkId) {
        Link link = links.remove(linkId);
        if (link == null) return;

        if (link.device != null) linkByAddress.remove(link.device.getAddress());
        synchronized (link) {
            link.outbound.clear();
            link.writing = false;
            link.ready = false;
        }

        try {
            if (link.gatt != null) {
                link.gatt.disconnect();
                link.gatt.close();
            } else if (gattServer != null && link.device != null) {
                gattServer.cancelConnection(link.device);
            }
        } catch (RuntimeException e) {
            debug("closeLink " + linkId + ": " + e.getMessage());
        }

        debug("link " + linkId + ": closed locally");
        onLinkDown(linkId, 0);
    }

    /**
     * Queues a frame. Chunking to the MTU and pacing against GATT callbacks happen here.
     *
     * @return false when the link is gone or has fallen too far behind.
     */
    public boolean send(int linkId, byte[] frame) {
        if (frame == null || frame.length == 0 || frame.length > MAX_FRAME_BYTES) return false;

        Link link = links.get(linkId);
        if (link == null) return false;

        // 2-byte big-endian length prefix, then the frame.
        byte[] framed = new byte[LENGTH_PREFIX_BYTES + frame.length];
        framed[0] = (byte) ((frame.length >> 8) & 0xFF);
        framed[1] = (byte) (frame.length & 0xFF);
        System.arraycopy(frame, 0, framed, LENGTH_PREFIX_BYTES, frame.length);

        synchronized (link) {
            int needed = (framed.length + link.chunkSize - 1) / link.chunkSize;
            if (link.outbound.size() + needed > MAX_QUEUED_CHUNKS) {
                Log.w(TAG, "link " + linkId + " is not draining; refusing frame");
                return false;
            }
            // Whole frame queued at once, so chunks of two frames never interleave.
            for (int offset = 0; offset < framed.length; offset += link.chunkSize) {
                int size = Math.min(link.chunkSize, framed.length - offset);
                link.outbound.add(Arrays.copyOfRange(framed, offset, offset + size));
            }
        }
        pump(link);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────

    /** Sends the next queued chunk if the link is idle. Called after every state change. */
    private void pump(final Link link) {
        synchronized (link) {
            if (link.writing || !link.ready) return;
            byte[] next = link.outbound.peek();
            if (next == null) return;

            if (writeChunk(link, next)) {
                link.writing = true;
                link.retries = 0;
                return;
            }
            if (++link.retries <= MAX_WRITE_RETRIES) {
                handler.postDelayed(() -> pump(link), WRITE_RETRY_DELAY_MS * link.retries);
                return;
            }
        }
        // Outside the monitor: closeLink reports upward.
        Log.w(TAG, "link " + link.linkId + " refused " + MAX_WRITE_RETRIES
                   + " writes in a row; closing it");
        closeLink(link.linkId);
    }

    /** A chunk's completion callback arrived. */
    private void onChunkSent(Link link, boolean success) {
        if (!success) {
            // The far end now holds part of a frame it will never complete.
            Log.w(TAG, "link " + link.linkId + ": chunk write failed; closing link");
            closeLink(link.linkId);
            return;
        }
        synchronized (link) {
            link.writing = false;
            link.outbound.poll();
        }
        pump(link);
    }

    private void markReady(Link link, boolean reportLinkUp) {
        synchronized (link) {
            if (link.ready) return;
            link.ready = true;
        }
        debug("link " + link.linkId + ": ready");
        if (reportLinkUp) onLinkUp(link.linkId, link.peerId != null ? link.peerId : new byte[8]);
        pump(link);
    }

    private boolean writeChunk(Link link, byte[] chunk) {
        try {
            if (link.gatt != null) {
                if (link.remoteRx == null) return false;
                // Central role: write to the peer's RX characteristic.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    return link.gatt.writeCharacteristic(
                                   link.remoteRx, chunk,
                                   BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
                           == BluetoothGatt.GATT_SUCCESS;
                }
                return legacyWriteCharacteristic(link.gatt, link.remoteRx, chunk);
            }

            // Peripheral role: notify the connected central on our TX characteristic.
            if (gattServer == null || txCharacteristic == null || link.device == null) {
                return false;
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                return gattServer.notifyCharacteristicChanged(link.device, txCharacteristic,
                                                              false, chunk)
                       == BluetoothGatt.GATT_SUCCESS;
            }
            return legacyNotify(gattServer, link.device, txCharacteristic, chunk);
        } catch (RuntimeException e) {
            debug("writeChunk on link " + link.linkId + ": " + e.getMessage());
            return false;
        }
    }

    // ── API 28-32 GATT calls ─────────────────────────────────────────────────
    //
    // Android 13 (API 33) replaced "set the value on the object, then write it" with
    // calls that take the bytes directly, and deprecated the old ones. We support API
    // 28+, so the old calls are still required below 33. Every call site checks
    // Build.VERSION.SDK_INT first; these helpers exist so the deprecation warning is
    // suppressed for exactly these calls and nowhere else. Delete them once minSdk
    // reaches 33.

    @SuppressWarnings("deprecation")
    private static boolean legacyWriteCharacteristic(BluetoothGatt gatt,
                                                     BluetoothGattCharacteristic characteristic,
                                                     byte[] value) {
        characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
        characteristic.setValue(value);
        return gatt.writeCharacteristic(characteristic);
    }

    @SuppressWarnings("deprecation")
    private static boolean legacyNotify(BluetoothGattServer server, BluetoothDevice device,
                                        BluetoothGattCharacteristic characteristic,
                                        byte[] value) {
        characteristic.setValue(value);
        return server.notifyCharacteristicChanged(device, characteristic, false);
    }

    @SuppressWarnings("deprecation")
    private static boolean legacyEnableNotifications(BluetoothGatt gatt,
                                                     BluetoothGattDescriptor cccd) {
        cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
        return gatt.writeDescriptor(cccd);
    }

    /**
     * Bytes we may put in one write on a link with this MTU.
     *
     * <p>Three bytes go to the ATT header, and the result is then clamped to the
     * specification's 512-octet attribute limit — MTU 517 would otherwise compute 514
     * and produce writes the peer silently truncates, desynchronising the frame stream.
     */
    private static int usableChunk(int mtu) {
        return Math.max(DEFAULT_CHUNK, Math.min(mtu - 3, MAX_ATT_VALUE));
    }

    /** Accumulates chunks and emits each complete frame exactly once. */
    private void acceptChunk(Link link, byte[] chunk) {
        synchronized (link) {
            byte[] merged = new byte[link.inbound.length + chunk.length];
            System.arraycopy(link.inbound, 0, merged, 0, link.inbound.length);
            System.arraycopy(chunk, 0, merged, link.inbound.length, chunk.length);
            link.inbound = merged;

            while (link.inbound.length >= LENGTH_PREFIX_BYTES) {
                int length = ((link.inbound[0] & 0xFF) << 8) | (link.inbound[1] & 0xFF);

                if (length <= 0 || length > MAX_FRAME_BYTES) {
                    // A nonsense length means the stream is desynchronised and there
                    // is no way to resync — drop the buffer rather than guess.
                    Log.w(TAG, "link " + link.linkId + ": bad frame length " + length
                               + ", resetting buffer");
                    link.inbound = new byte[0];
                    return;
                }
                if (link.inbound.length < LENGTH_PREFIX_BYTES + length) return;  // partial

                byte[] frame = Arrays.copyOfRange(link.inbound, LENGTH_PREFIX_BYTES,
                                                  LENGTH_PREFIX_BYTES + length);
                link.inbound = Arrays.copyOfRange(link.inbound, LENGTH_PREFIX_BYTES + length,
                                                  link.inbound.length);
                onFrame(link.linkId, frame);
            }
        }
    }

    private void publishService() {
        BluetoothGattService service =
                new BluetoothGattService(serviceUuid, BluetoothGattService.SERVICE_TYPE_PRIMARY);

        BluetoothGattCharacteristic rx = new BluetoothGattCharacteristic(
                RX_UUID,
                BluetoothGattCharacteristic.PROPERTY_WRITE
                        | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattCharacteristic.PERMISSION_WRITE);

        txCharacteristic = new BluetoothGattCharacteristic(
                TX_UUID,
                BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                BluetoothGattCharacteristic.PERMISSION_READ);
        txCharacteristic.addDescriptor(new BluetoothGattDescriptor(
                CCCD_UUID,
                BluetoothGattDescriptor.PERMISSION_READ
                        | BluetoothGattDescriptor.PERMISSION_WRITE));

        service.addCharacteristic(rx);
        service.addCharacteristic(txCharacteristic);
        gattServer.addService(service);
    }

    private Link linkForDevice(BluetoothDevice device, boolean createAsPeripheral) {
        Integer id = linkByAddress.get(device.getAddress());
        if (id != null) {
            Link existing = links.get(id);
            if (existing != null) return existing;
        }
        if (!createAsPeripheral) return null;

        Link link = new Link();
        link.linkId = nextLinkId.getAndIncrement();
        link.device = device;
        links.put(link.linkId, link);
        linkByAddress.put(device.getAddress(), link.linkId);
        return link;
    }

    private void registerAdapterReceiver() {
        if (receiverRegistered) return;
        IntentFilter filter = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                // System broadcasts are still delivered to a non-exported receiver.
                context.registerReceiver(adapterStateReceiver, filter,
                                         Context.RECEIVER_NOT_EXPORTED);
            } else {
                context.registerReceiver(adapterStateReceiver, filter);
            }
            receiverRegistered = true;
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot watch the Bluetooth switch", e);
        }
    }

    private void unregisterAdapterReceiver() {
        if (!receiverRegistered) return;
        try {
            context.unregisterReceiver(adapterStateReceiver);
        } catch (RuntimeException ignored) {
            // already gone
        }
        receiverRegistered = false;
    }

    // ── callbacks — ALL on binder threads ────────────────────────────────────

    /** Without this, switching Bluetooth off leaves C++ believing the radio is up, so it
     *  never retries — and switching it back on would change nothing until a restart. */
    private final BroadcastReceiver adapterStateReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent intent) {
            if (!BluetoothAdapter.ACTION_STATE_CHANGED.equals(intent.getAction())) return;
            int state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR);
            if (state != BluetoothAdapter.STATE_TURNING_OFF
                    && state != BluetoothAdapter.STATE_OFF) {
                return;
            }
            if (!started) return;

            Log.w(TAG, "bluetooth was switched off");
            stop();
            onRadioState(0);
        }
    };

    private final AdvertiseCallback advertiseCallback = new AdvertiseCallback() {
        @Override
        public void onStartSuccess(AdvertiseSettings settingsInEffect) {
            advertising = true;
            debug("advertising");
        }

        @Override
        public void onStartFailure(int errorCode) {
            if (errorCode == ADVERTISE_FAILED_ALREADY_STARTED) return;
            advertising = false;
            // Not reported as the radio going down: scanning and existing links still
            // work, and restarting the whole radio would not fix an advertising limit.
            Log.e(TAG, "advertising failed, error " + errorCode);
        }
    };

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            if (result.getScanRecord() == null) return;

            byte[] peerId = result.getScanRecord().getManufacturerSpecificData(MANUFACTURER_ID);
            if (peerId == null || peerId.length != 8) {
                // Advertising our service but no peer id: a different build, or a
                // scan response we have not received yet. Ignore until it arrives.
                return;
            }
            onPeerSeen(peerId, result.getDevice().getAddress(), result.getRssi());
        }

        @Override
        public void onScanFailed(int errorCode) {
            if (errorCode == SCAN_FAILED_ALREADY_STARTED) return;
            scanning = false;
            // Reported as the radio going down: the usual causes (registration failure,
            // internal error) clear only when the scanner is rebuilt, which is exactly
            // what the core's restart does.
            Log.e(TAG, "scan failed, error " + errorCode);
            onRadioState(0);
        }
    };

    private final BluetoothGattCallback gattClientCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            Link link = linkForDevice(gatt.getDevice(), false);
            if (link == null) return;

            if (newState == BluetoothProfile.STATE_CONNECTED
                    && status == BluetoothGatt.GATT_SUCCESS) {
                debug("link " + link.linkId + ": connected as central");
                // A bigger MTU means fewer chunks per frame. Discovery waits for
                // the MTU result so chunkSize is right before anything is sent.
                if (!gatt.requestMtu(PREFERRED_MTU)) gatt.discoverServices();
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED
                    || status != BluetoothGatt.GATT_SUCCESS) {
                links.remove(link.linkId);
                linkByAddress.remove(gatt.getDevice().getAddress());
                synchronized (link) {
                    link.outbound.clear();
                    link.writing = false;
                }
                gatt.close();
                debug("link " + link.linkId + ": disconnected, status " + status);
                onLinkDown(link.linkId, status == BluetoothGatt.GATT_SUCCESS ? 1 : 3);
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt gatt, int mtu, int status) {
            Link link = linkForDevice(gatt.getDevice(), false);
            if (link != null && status == BluetoothGatt.GATT_SUCCESS) {
                synchronized (link) {
                    link.chunkSize = usableChunk(mtu);
                }
                debug("link " + link.linkId + ": mtu " + mtu
                      + ", chunk " + link.chunkSize);
            }
            gatt.discoverServices();
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            Link link = linkForDevice(gatt.getDevice(), false);
            if (link == null) return;
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.w(TAG, "link " + link.linkId + ": service discovery failed, " + status);
                closeLink(link.linkId);
                return;
            }

            BluetoothGattService service = gatt.getService(serviceUuid);
            if (service == null) {
                Log.w(TAG, "link " + link.linkId + ": peer does not offer our service");
                closeLink(link.linkId);
                return;
            }

            link.remoteRx = service.getCharacteristic(RX_UUID);
            BluetoothGattCharacteristic tx = service.getCharacteristic(TX_UUID);
            if (link.remoteRx == null || tx == null) {
                Log.w(TAG, "link " + link.linkId + ": peer's service is incomplete");
                closeLink(link.linkId);
                return;
            }

            // Subscribe, or the peer can never send us anything. The link is reported up
            // only once the subscription is confirmed — the descriptor write occupies the
            // connection's one GATT operation slot, so a frame sent now would be refused.
            gatt.setCharacteristicNotification(tx, true);
            BluetoothGattDescriptor cccd = tx.getDescriptor(CCCD_UUID);
            boolean pending = false;
            if (cccd != null) {
                try {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        pending = gatt.writeDescriptor(
                                          cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                                  == BluetoothGatt.GATT_SUCCESS;
                    } else {
                        pending = legacyEnableNotifications(gatt, cccd);
                    }
                } catch (RuntimeException e) {
                    Log.w(TAG, "link " + link.linkId + ": subscribe failed", e);
                }
            }
            if (!pending) markReady(link, true);
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor,
                                      int status) {
            Link link = linkForDevice(gatt.getDevice(), false);
            if (link == null || !CCCD_UUID.equals(descriptor.getUuid())) return;
            if (status != BluetoothGatt.GATT_SUCCESS) {
                // We can still send; we just will not hear back on this link.
                Log.w(TAG, "link " + link.linkId + ": subscription refused, status " + status);
            }
            markReady(link, true);
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt,
                                          BluetoothGattCharacteristic characteristic,
                                          int status) {
            if (!RX_UUID.equals(characteristic.getUuid())) return;
            Link link = linkForDevice(gatt.getDevice(), false);
            if (link != null) onChunkSent(link, status == BluetoothGatt.GATT_SUCCESS);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt,
                                            BluetoothGattCharacteristic characteristic,
                                            byte[] value) {
            Link link = linkForDevice(gatt.getDevice(), false);
            if (link != null) acceptChunk(link, value);
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt gatt,
                                            BluetoothGattCharacteristic characteristic) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) return;  // handled above
            Link link = linkForDevice(gatt.getDevice(), false);
            if (link != null && characteristic.getValue() != null) {
                acceptChunk(link, characteristic.getValue());
            }
        }
    };

    private final BluetoothGattServerCallback gattServerCallback =
            new BluetoothGattServerCallback() {

        @Override
        public void onConnectionStateChange(BluetoothDevice device, int status, int newState) {
            String address = device.getAddress();

            if (newState == BluetoothProfile.STATE_CONNECTED) {
                // Our own outgoing connection to this device shows up here too: the
                // GATT server sees every LE connection. Treating it as a second,
                // incoming link would give one connection two identities.
                if (linkByAddress.containsKey(address)) return;

                Link link = linkForDevice(device, true);
                debug("link " + link.linkId + ": connected as peripheral");
                onLinkUp(link.linkId, new byte[8]);  // identity arrives with its ANNOUNCE
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Integer id = linkByAddress.get(address);
                if (id == null) return;
                Link link = links.get(id);
                if (link == null || link.gatt != null) return;  // central: reported elsewhere

                links.remove(id);
                linkByAddress.remove(address);
                synchronized (link) {
                    link.outbound.clear();
                    link.writing = false;
                }
                debug("link " + id + ": central disconnected");
                onLinkDown(id, 1);
            }
        }

        @Override
        public void onMtuChanged(BluetoothDevice device, int mtu) {
            Link link = linkForDevice(device, false);
            if (link == null) return;
            synchronized (link) {
                link.chunkSize = usableChunk(mtu);
            }
        }

        @Override
        public void onCharacteristicWriteRequest(BluetoothDevice device, int requestId,
                                                 BluetoothGattCharacteristic characteristic,
                                                 boolean preparedWrite, boolean responseNeeded,
                                                 int offset, byte[] value) {
            if (RX_UUID.equals(characteristic.getUuid())) {
                Link link = linkForDevice(device, true);
                if (value != null) acceptChunk(link, value);
            }
            if (responseNeeded && gattServer != null) {
                gattServer.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset,
                                        null);
            }
        }

        @Override
        public void onDescriptorWriteRequest(BluetoothDevice device, int requestId,
                                             BluetoothGattDescriptor descriptor,
                                             boolean preparedWrite, boolean responseNeeded,
                                             int offset, byte[] value) {
            if (responseNeeded && gattServer != null) {
                gattServer.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset,
                                        null);
            }

            // The central subscribing to our TX characteristic: from now on
            // notifications reach it, so queued frames can go.
            if (CCCD_UUID.equals(descriptor.getUuid()) && value != null
                    && Arrays.equals(value, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)) {
                Link link = linkForDevice(device, false);
                if (link != null) markReady(link, false);
            }
        }

        @Override
        public void onNotificationSent(BluetoothDevice device, int status) {
            Link link = linkForDevice(device, false);
            if (link != null) onChunkSent(link, status == BluetoothGatt.GATT_SUCCESS);
        }
    };

    // ── native callbacks, implemented in adapters/transport/ble ──────────────
    // These ENQUEUE onto the core thread and return immediately.

    private static native void onPeerSeen(byte[] peerId, String address, int rssi);

    private static native void onLinkUp(int linkId, byte[] peerId);

    private static native void onLinkDown(int linkId, int reason);

    private static native void onFrame(int linkId, byte[] frame);

    private static native void onRadioState(int state);
}
