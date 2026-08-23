package com.gantrping.rover

/**
 * v0.4.1: bridge between native OpenXR loop and Kotlin/Android APIs.
 * Native code JNI-calls into here for MediaProjection, VirtualDisplay, input injection etc.
 * For now: single hello method to prove the bridge works.
 */
object RoverBridge {
    @JvmStatic
    fun helloFromKotlin(): String {
        return "hello from Kotlin! rover_ui v0.4.1 bridge alive"
    }
}
