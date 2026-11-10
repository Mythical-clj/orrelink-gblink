// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.xdnetplay

/** The shared GB-Link manager (native), for the launcher card. */
object GBLinkBridge {
    @JvmStatic external fun getStatus(): String
    @JvmStatic external fun getAdapterCount(): Int
    @JvmStatic external fun setControllerEnabled(on: Boolean)
    @JvmStatic external fun isControllerEnabled(): Boolean
    @JvmStatic external fun setScreenMirror(on: Boolean)
    @JvmStatic external fun isScreenMirror(): Boolean
}
