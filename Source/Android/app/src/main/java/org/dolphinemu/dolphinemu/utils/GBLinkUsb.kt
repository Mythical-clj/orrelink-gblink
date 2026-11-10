// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import androidx.annotation.Keep
import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.DolphinApplication

/**
 * USB access for GB-Link adapters. The shared GB-Link code (UsbLink.cpp) calls the @Keep functions
 * below from its own thread; everything Android-specific (permission, opening, endpoints) lives here.
 */
object GBLinkUsb {
    private val VIDS = intArrayOf(0x2FE3, 0xCAFE, 0x239A)
    private val ACTION_PERMISSION = BuildConfig.APPLICATION_ID + ".GBLINK_USB_PERMISSION"

    private class Open(
        val connection: UsbDeviceConnection,
        val usbInterface: UsbInterface,
        val endpoints: Map<Int, UsbEndpoint>
    )

    private val lock = Any()
    private val open = HashMap<String, Open>()
    private val asked = HashSet<String>()

    @JvmStatic
    external fun nativeInit()

    /** Call once after the native library is loaded. */
    @JvmStatic
    fun init() {
        nativeInit()
    }

    private fun manager(): UsbManager =
        DolphinApplication.getAppContext().getSystemService(Context.USB_SERVICE) as UsbManager

    private fun isGbLink(device: UsbDevice) = VIDS.contains(device.vendorId)

    private fun requestPermission(device: UsbDevice) {
        val context = DolphinApplication.getAppContext()
        val intent = Intent(ACTION_PERMISSION).setPackage(context.packageName)
        val pending = PendingIntent.getBroadcast(context, 0, intent, PendingIntent.FLAG_IMMUTABLE)
        manager().requestPermission(device, pending)
    }

    // "2.27" (bcdDevice as UsbDevice.version prints it) -> 0x0227
    private fun bcd(version: String?): Int {
        val parts = (version ?: "0.0").split(".")
        val major = parts.getOrNull(0)?.toIntOrNull(16) ?: 0
        val minor = parts.getOrNull(1)?.toIntOrNull(16) ?: 0
        return (major shl 8) or minor
    }

    /** One "name|vid|pid|bcd|product" per adapter we may open. Asks for permission as needed. */
    @Keep
    @JvmStatic
    fun list(): Array<String> {
        val mgr = manager()
        val out = ArrayList<String>()
        val present = HashSet<String>()
        for (device in mgr.deviceList.values) {
            if (!isGbLink(device)) continue
            present.add(device.deviceName)
            if (!mgr.hasPermission(device)) {
                // Ask once per plug-in; the next poll sees the result.
                if (asked.add(device.deviceName)) requestPermission(device)
                continue
            }
            out.add(
                "${device.deviceName}|${device.vendorId}|${device.productId}|" +
                    "${bcd(device.version)}|${device.productName ?: "GB-Link"}"
            )
        }
        asked.retainAll(present)
        return out.toTypedArray()
    }

    /** Opens and claims the adapter. Returns [iface, cmdOut, cmdIn, dataOut, dataIn, 0, 0, 0]. */
    @Keep
    @JvmStatic
    fun open(name: String): IntArray? {
        val mgr = manager()
        val device = mgr.deviceList[name] ?: return null
        if (!mgr.hasPermission(device)) return null
        synchronized(lock) { closeLocked(name) }
        val connection = mgr.openDevice(device) ?: return null

        var chosen: UsbInterface? = null
        for (i in 0 until device.interfaceCount) {
            val itf = device.getInterface(i)
            if (itf.interfaceClass != 0xFF) continue
            val bulk = (0 until itf.endpointCount).map { itf.getEndpoint(it) }
                .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
            if (bulk.any { it.direction == UsbConstants.USB_DIR_IN } &&
                bulk.any { it.direction == UsbConstants.USB_DIR_OUT }
            ) {
                chosen = itf
                break
            }
        }
        if (chosen == null || !connection.claimInterface(chosen, true)) {
            connection.close()
            return null
        }
        val bulk = (0 until chosen.endpointCount).map { chosen.getEndpoint(it) }
            .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        val ins = bulk.filter { it.direction == UsbConstants.USB_DIR_IN }.sortedBy { it.address }
        val outs = bulk.filter { it.direction == UsbConstants.USB_DIR_OUT }.sortedBy { it.address }

        val result = IntArray(8)
        result[0] = chosen.id
        if (device.vendorId == 0x2FE3 && ins.size >= 2 && outs.size >= 2) {
            result[1] = outs[0].address
            result[2] = ins[0].address
            result[3] = outs[1].address
            result[4] = ins[1].address
        } else {
            result[3] = outs[0].address
            result[4] = ins[0].address
            // Old firmware: CDC-style SET_CONTROL_LINE_STATE (DTR).
            connection.controlTransfer(0x21, 0x22, 0x01, chosen.id, null, 0, 1000)
        }
        synchronized(lock) {
            open[name] = Open(connection, chosen, bulk.associateBy { it.address })
        }
        return result
    }

    /** Bytes moved, or a negative number: -1 timeout/failure, -2 adapter gone. */
    @Keep
    @JvmStatic
    fun bulk(name: String, endpoint: Int, buffer: ByteArray, length: Int, timeoutMs: Int): Int {
        val entry = synchronized(lock) { open[name] } ?: return -2
        val ep = entry.endpoints[endpoint] ?: return -1
        val moved = entry.connection.bulkTransfer(ep, buffer, length, timeoutMs)
        if (moved < 0 && !manager().deviceList.containsKey(name)) return -2
        return moved
    }

    @Keep
    @JvmStatic
    fun close(name: String) {
        synchronized(lock) { closeLocked(name) }
    }

    private fun closeLocked(name: String) {
        val entry = open.remove(name) ?: return
        try {
            entry.connection.releaseInterface(entry.usbInterface)
        } catch (_: Exception) {
        }
        entry.connection.close()
    }
}
