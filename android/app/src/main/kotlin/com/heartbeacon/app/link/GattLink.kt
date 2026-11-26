package com.heartbeacon.app.link

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.util.Log
import java.util.UUID

/**
 * The one GATT client to the beacon (PROTOCOL.md section 6). Frames in and out are opaque here;
 * :core's RelayEngine splits and reassembles them.
 *
 * Plan: the first connect after adding uses autoConnect=false; after that one autoConnect=true GATT is
 * kept open and never closed on link loss, so Android reconnects when the beacon advertises again.
 * The owner recreates it when Bluetooth comes back on ([close] + [open]).
 */
@SuppressLint("MissingPermission") // BeaconService only runs after BLUETOOTH_CONNECT is granted
class GattLink(private val ctx: Context, private val mac: String, private val listener: Listener) {
    interface Listener {
        /** Link usable (connected, discovered, MTU set, TX notifications on) or lost. */
        fun onLink(up: Boolean, mtu: Int)
        fun onFrame(frame: ByteArray)
        /** TX subscription refused: the beacon no longer knows this phone (factory reset, or its 4th bond). */
        fun onBondLost() {}
    }

    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private val queue = ArrayDeque<ByteArray>()
    private var writing = false
    private var mtu = 23

    fun open(autoConnect: Boolean) {
        val dev = ctx.getSystemService(BluetoothManager::class.java)?.adapter?.getRemoteDevice(mac) ?: return
        gatt = dev.connectGatt(ctx, autoConnect, callback, BluetoothDevice.TRANSPORT_LE)
    }

    fun close() = synchronized(this) {
        gatt?.close()
        gatt = null
        rx = null
        queue.clear()
        writing = false
    }

    /** Queue frames for RX; one write with response in flight at a time, in order. */
    fun send(frames: List<ByteArray>) = synchronized(this) {
        queue.addAll(frames)
        pump()
    }

    private fun pump() {
        if (writing) return
        val g = gatt ?: return
        val c = rx ?: return
        val f = queue.removeFirstOrNull() ?: return
        writing = true
        // Fragments with more to follow go without response (one connection event per message instead of one per
        // frame); the last one is acknowledged. Only if the beacon's RX allows it (firmware from 2026-11-26 on).
        val more = f.isNotEmpty() && f[0].toInt() and 0x80 != 0
        val noRsp = more && c.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0
        val type = if (noRsp) BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE else BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        val rc = g.writeCharacteristic(c, f, type)
        if (rc != BluetoothStatusCodes.SUCCESS) {
            Log.w(TAG, "write refused ($rc); dropping the rest of this burst")
            queue.clear() // the beacon's reassembler drops the partial; the relay's timers cover the loss
            writing = false
        }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, state: Int) {
            if (state == BluetoothProfile.STATE_CONNECTED) {
                g.discoverServices()
                return
            }
            synchronized(this@GattLink) {
                rx = null
                queue.clear()
                writing = false
            }
            listener.onLink(false, 23)
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS || g.getService(SERVICE) == null) {
                Log.w(TAG, "no beacon service (status $status)")
                g.disconnect()
                return
            }
            g.requestMtu(247)
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            this@GattLink.mtu = if (status == BluetoothGatt.GATT_SUCCESS) mtu else 23
            val tx = g.getService(SERVICE)?.getCharacteristic(TX) ?: return
            g.setCharacteristicNotification(tx, true)
            // TX needs encryption: this write is what triggers LE Secure Connections bonding on a new phone.
            val cccd = tx.getDescriptor(CCCD) ?: return
            g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                // Not disconnect(): on an autoConnect GATT that cancels reconnecting for good. The owner closes
                // this GATT, says so in the app, and opens a fresh one later.
                Log.w(TAG, "CCCD write failed ($status): bond or encryption problem")
                listener.onBondLost()
                return
            }
            synchronized(this@GattLink) { rx = g.getService(SERVICE)?.getCharacteristic(RX) }
            listener.onLink(true, mtu)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (c.uuid == TX) listener.onFrame(value)
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) =
            synchronized(this@GattLink) {
                writing = false
                pump()
            }
    }

    companion object {
        private const val TAG = "GattLink"
        val SERVICE: UUID = UUID.fromString("48420001-6265-6163-6F6E-763200000000")
        val RX: UUID = UUID.fromString("48420002-6265-6163-6F6E-763200000000")
        val TX: UUID = UUID.fromString("48420003-6265-6163-6F6E-763200000000")
        private val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
